// ============================================================================
// scrfd_face_detector.cpp —— SCRFD 人脸检测实现（anchor-free、无显式 prior）
// ============================================================================
// 解码思路（与 retinaface/scrfd 官方 postprocess 对应）：
//   模型每个 FPN 层级输出 score(每格 2 anchor 的 1 通道置信度) 与
//   bbox(每格 2 anchor 的 4 通道"参考点到左/上/右/下四边距离")；
//   参考点取网格左上角 (x*stride, y*stride)，因此无需预生成 prior box 表，
//   解码即用即算（见 detectRgba 中 scales[] 与四距离还原公式）。
// ============================================================================
#include "scrfd_face_detector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>
#include <opencv2/imgproc.hpp>

namespace {
// 模型固定输入边长（训练分辨率 640x640，三层级网格 80/40/20 = 640/stride）
constexpr int kInputSize = 640;

// 单个候选框：坐标已是"源图像素系"（完成 /scale 逆变换与 clamp）
struct Candidate {
    float x1, y1, x2, y2, score;
};

// IoU = 交集面积/并集面积，O(1)。约定用连续坐标直算 (x2-x1)，不加 1 像素，
// 与 YOLO 流水线 CalculateOverlap 的离散 +1 记法不同（仅影响临界值微小偏移）
float iou(const Candidate& a, const Candidate& b)
{
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);
    const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
    const float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    const float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    const float denominator = area_a + area_b - intersection;
    return denominator > 0.0f ? intersection / denominator : 0.0f;
}

// 经典贪心 NMS：先按 score 降序排序（O(N log N)），再自高分向低分扫描，
// 与每个未抑制的更低分框算 IoU，超阈值者标记抑制——两两比较最坏 O(N²)。
// 人脸场景 N 通常 < 几十，无需 yolo11 那套"按类别分桶再 NMS"的优化
// （本模型只有人脸单一类别，天然免分桶）。
// 形参按值接收 candidates：调用点用 std::move 传入，避免整表再拷一份。
std::vector<Candidate> nms(std::vector<Candidate> candidates, float threshold)
{
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    std::vector<Candidate> result;
    std::vector<bool> suppressed(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(candidates[i]);
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!suppressed[j] && iou(candidates[i], candidates[j]) > threshold)
                suppressed[j] = true;
        }
    }
    return result;
}
} // namespace

ScrfdFaceDetector::~ScrfdFaceDetector()
{
    release();
}

// ============================================================================
// init —— 读整个 .rknn 文件并创建 NPU 上下文
// ============================================================================
// 要点：
//   - 先 release()：允许对同一对象重复 init 换模型而不泄漏旧上下文；
//   - rknn_init 会把模型字节流拷入驱动侧，栈/堆上的 model vector 随后即可释放；
//   - set_core_mask 失败时回滚 release()，保证"返回 false ⇒ 无半成品上下文"
//     的强异常约定（调用方 CameraPreviewDecoder 依此丢弃该检测器槽位）。
// ============================================================================
bool ScrfdFaceDetector::init(const std::string& model_path, rknn_core_mask core_mask)
{
    release();
    // ate 模式打开以便 tellg 直接得到文件长度，一次性读入
    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> model(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(model.data()), size)) return false;
    if (rknn_init(&context_, model.data(), static_cast<uint32_t>(model.size()), 0, nullptr) < 0)
        return false;
    if (rknn_set_core_mask(context_, core_mask) < 0) {
        release();
        return false;
    }
    return true;
}

// 幂等释放：析构函数直接调用，重复调用安全（context_ 置 0 作哨兵）
void ScrfdFaceDetector::release()
{
    if (context_ != 0) {
        rknn_destroy(context_);
        context_ = 0;
    }
}

// ============================================================================
// detectRgba —— RGBA 帧 → 人脸框（同步全链路：预处理→rknn_run→解码→NMS）
// ============================================================================
// 整体复杂度：O(H*W*A) 网格扫描（640² 下三层共 80²+40²+20²=8400 格 ×2 anchor）
//            + O(N log N + N²) NMS。单次调用即占满解码线程一个帧间隙，
//            因此上层以"每 5 帧一次"降频。
// 失败语义：仅"上下文缺失/推理链路出错"返回 false；"无人脸"返回 true 且
// faces 为空——调用方据此区分错误与空场景。
// ============================================================================
bool ScrfdFaceDetector::detectRgba(const uint8_t* pixels, int width, int height, int stride,
                                   std::vector<ScrfdFaceBox>& faces,
                                   float score_threshold, float nms_threshold)
{
    faces.clear();
    // 入参防御：stride 至少容纳一行 RGBA(width*4)，否则后续按行寻址会越界；
    // cv::Mat 仅包装外部内存（零拷贝、不持有权），生命周期由调用帧保证
    if (!isInitialized() || !pixels || width <= 0 || height <= 0 || stride < width * 4)
        return false;

    cv::Mat rgba(height, width, CV_8UC4, const_cast<uint8_t*>(pixels), stride);
    cv::Mat bgr;
    cv::cvtColor(rgba, bgr, cv::COLOR_RGBA2BGR);

    // ---- letterbox（左上对齐版）----
    // scale 取宽高比中的较小值保证整图放入；cv::Mat::zeros 先铺 640x640 黑底，
    // 再把缩放结果贴到左上角 (0,0)：与 YOLO 主流水线"居中 + 灰 114"不同，
    // 右下留黑边使坐标逆变换没有平移项，只需乘 1/scale。
    // round 而非截断：避免 0.5px 级系统偏差；两侧 max(1,min(640,...)) 防
    // 极端分辨率下退化为 0 宽/高。
    const float scale = std::min(kInputSize / static_cast<float>(width),
                                 kInputSize / static_cast<float>(height));
    const int resized_width = std::max(1, std::min(kInputSize, static_cast<int>(std::round(width * scale))));
    const int resized_height = std::max(1, std::min(kInputSize, static_cast<int>(std::round(height * scale))));
    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(resized_width, resized_height), 0, 0, cv::INTER_LINEAR);
    cv::Mat canvas = cv::Mat::zeros(kInputSize, kInputSize, CV_8UC3);
    resized.copyTo(canvas(cv::Rect(0, 0, resized_width, resized_height)));
    cv::Mat rgb;
    cv::cvtColor(canvas, rgb, cv::COLOR_BGR2RGB);

    // ---- 归一化：(px - 127.5) / 128 → [-1, 1] ----
    // 该模型导出时输入声明为 float 且按 [-1,1] 训练；逐字节按 NHWC 顺序
    // 展开（rgb.data 连续、无行 padding，640*3 步进），与下方
    // RKNN_TENSOR_NHWC 声明一致。
    std::vector<float> input_data(static_cast<size_t>(kInputSize) * kInputSize * 3);
    for (size_t i = 0; i < input_data.size(); ++i)
        input_data[i] = (static_cast<float>(rgb.data[i]) - 127.5f) / 128.0f;

    // index=0：本模型唯一输入张量；buf 指针在 rknn_inputs_set 内部即被
    // 拷贝/注册，input_data 出作用域前推理早已完成，无悬垂风险。
    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<uint32_t>(input_data.size() * sizeof(float));
    input.buf = input_data.data();
    if (rknn_inputs_set(context_, 1, &input) < 0 || rknn_run(context_, nullptr) < 0)
        return false;   // 此处失败尚未获取输出，无泄漏路径

    // want_float=1：由 RKNN runtime 在 outputs_get 时完成反量化
    // (q - zp) * scale，后处理免写量化分支（代价是驱动侧多一次反量化，
    // 对轻量人脸模型可接受）
    rknn_output outputs[9]{};
    for (auto& output : outputs) output.want_float = 1;
    if (rknn_outputs_get(context_, 9, outputs, nullptr) < 0)
        return false;

    // ---- 输出张量布局（按导出顺序硬约定，换模型需同步核对）----
    //   [0][1][2]   = stride 8/16/32 的 score 图（每格 2 anchor ×1 通道）
    //   [3][4][5]   = 对应层级的 bbox 图（每格 2 anchor ×4 通道 l/t/r/b 距离）
    //   [6][7][8]   = 对应层级的 kps 关键点图（本用途不消费）
    // grid = 640/stride；score/bbox 通道数 = grid*grid*2(*4)。
    struct Scale { int stride; int grid; int score; int bbox; };
    constexpr Scale scales[] = {{8, 80, 0, 3}, {16, 40, 1, 4}, {32, 20, 2, 5}};
    std::vector<Candidate> candidates;
    const float inverse_scale = 1.0f / scale;
    for (const auto& current : scales) {
        const float* score_data = static_cast<const float*>(outputs[current.score].buf);
        const float* bbox_data = static_cast<const float*>(outputs[current.bbox].buf);
        // ---- anchor-free 解码（无显式 prior box）----
        // 参考点 = 网格左上角 (x*stride, y*stride)；bbox 四通道是该点到
        // 左/上/右/下四边的距离（单位=格，故 ×stride 化像素）：
        //   x1 = (cx - l*stride)/scale, y1 = (cy - t*stride)/scale,
        //   x2 = (cx + r*stride)/scale, y2 = (cy + b*stride)/scale
        // score 索引 (y*grid+x)*2+anchor 即 NHWC [1,G,G,2] 展平。
        // 先阈值粗筛再解码框，可省掉绝大多数背景格的浮点运算。
        for (int y = 0; y < current.grid; ++y) {
            for (int x = 0; x < current.grid; ++x) {
                for (int anchor = 0; anchor < 2; ++anchor) {
                    const int index = (y * current.grid + x) * 2 + anchor;
                    const float score = score_data[index];
                    if (score < score_threshold) continue;
                    const float cx = static_cast<float>(x * current.stride);
                    const float cy = static_cast<float>(y * current.stride);
                    Candidate candidate{
                        (cx - bbox_data[index * 4 + 0] * current.stride) * inverse_scale,
                        (cy - bbox_data[index * 4 + 1] * current.stride) * inverse_scale,
                        (cx + bbox_data[index * 4 + 2] * current.stride) * inverse_scale,
                        (cy + bbox_data[index * 4 + 3] * current.stride) * inverse_scale,
                        score};
                    // 回归框可越出画面（黑边区域也会出响应）：clamp 回源图
                    // 像素域并丢弃退化框（x2<=x1），保证上层 cv::Rect 合法
                    candidate.x1 = std::clamp(candidate.x1, 0.0f, static_cast<float>(width - 1));
                    candidate.y1 = std::clamp(candidate.y1, 0.0f, static_cast<float>(height - 1));
                    candidate.x2 = std::clamp(candidate.x2, 0.0f, static_cast<float>(width - 1));
                    candidate.y2 = std::clamp(candidate.y2, 0.0f, static_cast<float>(height - 1));
                    if (candidate.x2 > candidate.x1 && candidate.y2 > candidate.y1)
                        candidates.push_back(candidate);
                }
            }
        }
    }
    // candidates 已持有纯 float 拷贝，故先释放驱动侧输出缓冲再做 NMS，
    // 缩短 NPU 输出内存占用窗口（NMS 不依赖 outputs）
    rknn_outputs_release(context_, 9, outputs);

    // 转整数框：截断取整（非四舍五入），左边界内收、宽高外扩至少 1px，
    // 对 2px 级抖动不敏感，点名框选够用
    for (const Candidate& candidate : nms(std::move(candidates), nms_threshold)) {
        faces.push_back({cv::Rect(static_cast<int>(candidate.x1), static_cast<int>(candidate.y1),
                                  static_cast<int>(candidate.x2 - candidate.x1),
                                  static_cast<int>(candidate.y2 - candidate.y1)),
                         candidate.score});
    }
    return true;
}
