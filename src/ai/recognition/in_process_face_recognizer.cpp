// ============================================================================
// in_process_face_recognizer.cpp —— 进程内 SCRFD 检测 + ArcFace 识别实现
// ============================================================================
// 推理链逐段移植自 tools/face_recognition/main.cpp（已板端验证的 SCRFD 9 输出
// 解析 + Umeyama 五点相似变换对齐 + 512 维 L2 归一化特征），差别仅在：
//   - 模型加载从"每次 main() 现 load"变为你构造时 init 一次、常驻复用；
//   - 输出不再写 JSON，而是直接填充 FaceDetectionResult 内存结构。
// 数值口径与原 exe 严格一致（同一 det/rec 权重下特征逐维可比），保证与旧
// 子进程链路注册库兼容——对照回归见 test_rollcall。
// ============================================================================
#include "in_process_face_recognizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace {

constexpr int DET_SIZE = 640;
constexpr int REC_SIZE = 112;
constexpr int FEATURE_DIM = 512;

// ArcFace 112x112 标准五点模板（与 exe 一致，勿改数值）
constexpr float ARCFACE_DST[5][2] = {
    {38.2946f, 51.6963f},
    {73.5318f, 51.5014f},
    {56.0252f, 71.7366f},
    {41.5493f, 92.3655f},
    {70.7299f, 92.2041f}
};

void l2_normalize(float* feat, int dim) {
    double sum = 0.0;
    for (int i = 0; i < dim; ++i) {
        sum += (double)feat[i] * (double)feat[i];
    }
    float norm = (float)std::sqrt(sum);
    if (norm > 1e-12f) {
        for (int i = 0; i < dim; ++i) feat[i] /= norm;
    }
}

float iou(const InProcessFaceBox& a, const InProcessFaceBox& b) {
    float inter_x1 = std::max(a.x1, b.x1);
    float inter_y1 = std::max(a.y1, b.y1);
    float inter_x2 = std::min(a.x2, b.x2);
    float inter_y2 = std::min(a.y2, b.y2);
    float inter_w = std::max(0.0f, inter_x2 - inter_x1);
    float inter_h = std::max(0.0f, inter_y2 - inter_y1);
    float inter_area = inter_w * inter_h;
    float area_a = std::max(0.0f, a.x2 - a.x1) * std::max(0.0f, a.y2 - a.y1);
    float area_b = std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
    float denom = area_a + area_b - inter_area;
    return denom > 0.0f ? inter_area / denom : 0.0f;
}

std::vector<InProcessFaceBox> nms(std::vector<InProcessFaceBox> boxes,
                                            float threshold) {
    std::sort(boxes.begin(), boxes.end(),
              [](const InProcessFaceBox& a, const InProcessFaceBox& b) {
                  return a.score > b.score;
              });
    std::vector<InProcessFaceBox> result;
    std::vector<bool> suppressed(boxes.size(), false);
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (suppressed[i]) continue;
        result.push_back(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (suppressed[j]) continue;
            if (iou(boxes[i], boxes[j]) > threshold) suppressed[j] = true;
        }
    }
    return result;
}

// 保持原图比例缩放到 640x640 左上角，其余补 0（与 exe 一致的 letterbox 方案）
struct DetPreprocessResult {
    cv::Mat input_rgb;
    float scale;
};

DetPreprocessResult preprocess_detection(const cv::Mat& bgr) {
    DetPreprocessResult result;
    float sx = (float)DET_SIZE / (float)bgr.cols;
    float sy = (float)DET_SIZE / (float)bgr.rows;
    result.scale = std::min(sx, sy);

    int resized_w = std::max(1, std::min(DET_SIZE, (int)std::round(bgr.cols * result.scale)));
    int resized_h = std::max(1, std::min(DET_SIZE, (int)std::round(bgr.rows * result.scale)));

    cv::Mat resized;
    cv::resize(bgr, resized, cv::Size(resized_w, resized_h), 0, 0, cv::INTER_LINEAR);
    cv::Mat canvas = cv::Mat::zeros(DET_SIZE, DET_SIZE, CV_8UC3);
    resized.copyTo(canvas(cv::Rect(0, 0, resized_w, resized_h)));
    cv::cvtColor(canvas, result.input_rgb, cv::COLOR_BGR2RGB);
    return result;
}

// SCRFD 9 输出解析：score=0/1/2, bbox=3/4/5, kps=6/7/8，每格 2 anchor，
// anchor 点 (j*stride, i*stride)，kps 10 值 [x0,y0,...,x4,y4]（与 exe 逐字一致）
std::vector<InProcessFaceBox> parse_scrfd_outputs(
    rknn_output* outputs, int orig_width, int orig_height,
    float det_scale, float score_threshold) {

    std::vector<InProcessFaceBox> boxes;
    struct ScaleInfo { int stride, grid_h, grid_w, num_anchors, score_idx, bbox_idx, kps_idx; };
    const ScaleInfo scales[3] = {
        {8,  80, 80, 2, 0, 3, 6},
        {16, 40, 40, 2, 1, 4, 7},
        {32, 20, 20, 2, 2, 5, 8}
    };
    const float inv_scale = 1.0f / det_scale;

    for (const auto& s : scales) {
        const float* score_data = (const float*)outputs[s.score_idx].buf;
        const float* bbox_data  = (const float*)outputs[s.bbox_idx].buf;
        const float* kps_data   = (const float*)outputs[s.kps_idx].buf;

        for (int i = 0; i < s.grid_h; ++i) {
            for (int j = 0; j < s.grid_w; ++j) {
                for (int a = 0; a < s.num_anchors; ++a) {
                    int idx = (i * s.grid_w + j) * s.num_anchors + a;
                    float score = score_data[idx];
                    if (score < score_threshold) continue;

                    float cx = (float)(j * s.stride);
                    float cy = (float)(i * s.stride);
                    float left   = bbox_data[idx * 4 + 0] * s.stride;
                    float top    = bbox_data[idx * 4 + 1] * s.stride;
                    float right  = bbox_data[idx * 4 + 2] * s.stride;
                    float bottom = bbox_data[idx * 4 + 3] * s.stride;

                    InProcessFaceBox box{};
                    box.score = score;
                    box.x1 = (cx - left)   * inv_scale;
                    box.y1 = (cy - top)    * inv_scale;
                    box.x2 = (cx + right)  * inv_scale;
                    box.y2 = (cy + bottom) * inv_scale;

                    box.x1 = std::max(0.0f, std::min(box.x1, (float)(orig_width  - 1)));
                    box.y1 = std::max(0.0f, std::min(box.y1, (float)(orig_height - 1)));
                    box.x2 = std::max(0.0f, std::min(box.x2, (float)(orig_width  - 1)));
                    box.y2 = std::max(0.0f, std::min(box.y2, (float)(orig_height - 1)));

                    for (int k = 0; k < 5; ++k) {
                        float dx = kps_data[idx * 10 + k * 2 + 0] * s.stride;
                        float dy = kps_data[idx * 10 + k * 2 + 1] * s.stride;
                        box.landmarks[k][0] = (cx + dx) * inv_scale;
                        box.landmarks[k][1] = (cy + dy) * inv_scale;
                    }
                    boxes.push_back(box);
                }
            }
        }
    }
    return boxes;
}

// Umeyama 五点相似变换（与 exe 采用的对齐一致，返回 2x3 CV_64F 仿射矩阵）
bool estimate_similarity_umeyama(const float src_pts[5][2], cv::Mat& M) {
    static const double dst_pts[5][2] = {
        {38.2946, 51.6963}, {73.5318, 51.5014}, {56.0252, 71.7366},
        {41.5493, 92.3655}, {70.7299, 92.2041}
    };
    const int N = 5, D = 2;
    cv::Mat src(N, D, CV_64F), dst(N, D, CV_64F);
    for (int i = 0; i < N; ++i) {
        src.at<double>(i, 0) = src_pts[i][0];
        src.at<double>(i, 1) = src_pts[i][1];
        dst.at<double>(i, 0) = dst_pts[i][0];
        dst.at<double>(i, 1) = dst_pts[i][1];
    }

    cv::Mat src_mean, dst_mean;
    cv::reduce(src, src_mean, 0, cv::REDUCE_AVG, CV_64F);
    cv::reduce(dst, dst_mean, 0, cv::REDUCE_AVG, CV_64F);

    cv::Mat src_demean = src.clone(), dst_demean = dst.clone();
    for (int i = 0; i < N; ++i) {
        src_demean.at<double>(i, 0) -= src_mean.at<double>(0, 0);
        src_demean.at<double>(i, 1) -= src_mean.at<double>(0, 1);
        dst_demean.at<double>(i, 0) -= dst_mean.at<double>(0, 0);
        dst_demean.at<double>(i, 1) -= dst_mean.at<double>(0, 1);
    }

    cv::Mat A = (dst_demean.t() * src_demean) / static_cast<double>(N);
    double src_var = 0.0;
    for (int i = 0; i < N; ++i) {
        double x = src_demean.at<double>(i, 0), y = src_demean.at<double>(i, 1);
        src_var += x * x + y * y;
    }
    src_var /= static_cast<double>(N);
    if (src_var <= std::numeric_limits<double>::epsilon()) return false;

    cv::SVD svd(A, cv::SVD::FULL_UV);
    cv::Mat U = svd.u, Vt = svd.vt, S = svd.w;
    double d0 = 1.0, d1 = 1.0;
    if (cv::determinant(A) < 0.0) d1 = -1.0;
    cv::Mat Dm = (cv::Mat_<double>(2, 2) << d0, 0.0, 0.0, d1);
    cv::Mat R = U * Dm * Vt;
    double s0 = S.at<double>(0, 0), s1 = S.at<double>(1, 0);
    double scale = (s0 * d0 + s1 * d1) / src_var;

    cv::Mat src_mean_col = (cv::Mat_<double>(2, 1) << src_mean.at<double>(0, 0), src_mean.at<double>(0, 1));
    cv::Mat dst_mean_col = (cv::Mat_<double>(2, 1) << dst_mean.at<double>(0, 0), dst_mean.at<double>(0, 1));
    cv::Mat t = dst_mean_col - scale * R * src_mean_col;

    M = cv::Mat::zeros(2, 3, CV_64F);
    M.at<double>(0, 0) = scale * R.at<double>(0, 0);
    M.at<double>(0, 1) = scale * R.at<double>(0, 1);
    M.at<double>(1, 0) = scale * R.at<double>(1, 0);
    M.at<double>(1, 1) = scale * R.at<double>(1, 1);
    M.at<double>(0, 2) = t.at<double>(0, 0);
    M.at<double>(1, 2) = t.at<double>(1, 0);
    return true;
}

bool align_face(const cv::Mat& img, const float landmarks[5][2], cv::Mat& aligned) {
    cv::Mat M;
    if (!estimate_similarity_umeyama(landmarks, M)) return false;
    cv::warpAffine(img, aligned, M, cv::Size(REC_SIZE, REC_SIZE),
                   cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
    return !aligned.empty();
}

// 一次性读入 .rknn 并建立上下文 + 绑核；失败不留半成品（调用方负责回收另一侧）
bool load_context(const std::string& model_path, rknn_core_mask core_mask, rknn_context* ctx) {
    std::ifstream file(model_path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const std::streamsize size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> model(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(model.data()), size)) return false;
    if (rknn_init(ctx, model.data(), static_cast<uint32_t>(model.size()), 0, nullptr) < 0)
        return false;
    if (rknn_set_core_mask(*ctx, core_mask) < 0) {
        rknn_destroy(*ctx);
        *ctx = 0;
        return false;
    }
    return true;
}

}  // namespace

InProcessFaceRecognizer::~InProcessFaceRecognizer() {
    release();
}

bool InProcessFaceRecognizer::init(const std::string& det_model_path,
                                   const std::string& rec_model_path,
                                   rknn_core_mask core_mask) {
    release();
    if (!load_context(det_model_path, core_mask, &det_ctx_)) return false;
    if (!load_context(rec_model_path, core_mask, &rec_ctx_)) {
        release();
        return false;
    }
    return true;
}

void InProcessFaceRecognizer::release() {
    if (det_ctx_ != 0) { rknn_destroy(det_ctx_); det_ctx_ = 0; }
    if (rec_ctx_ != 0) { rknn_destroy(rec_ctx_); rec_ctx_ = 0; }
}

bool InProcessFaceRecognizer::detectFaces(const cv::Mat& bgr,
                                         std::vector<InProcessFaceBox>& faces) {
    DetPreprocessResult prep = preprocess_detection(bgr);

    std::vector<float> input_f32(DET_SIZE * DET_SIZE * 3);
    const float norm_scale = 1.0f / 128.0f;
    const size_t total = prep.input_rgb.total() * prep.input_rgb.channels();
    for (size_t i = 0; i < total; ++i) {
        input_f32[i] = ((float)prep.input_rgb.data[i] - 127.5f) * norm_scale;
    }

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size = (uint32_t)(input_f32.size() * sizeof(float));
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_f32.data();
    input.pass_through = 0;

    if (rknn_inputs_set(det_ctx_, 1, &input) < 0) return false;
    if (rknn_run(det_ctx_, nullptr) < 0) return false;

    rknn_output outputs[9];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < 9; ++i) outputs[i].want_float = 1;
    if (rknn_outputs_get(det_ctx_, 9, outputs, nullptr) < 0) return false;

    std::vector<InProcessFaceBox> raw = parse_scrfd_outputs(
        outputs, bgr.cols, bgr.rows, prep.scale, det_threshold_);
    rknn_outputs_release(det_ctx_, 9, outputs);

    std::vector<InProcessFaceBox> kept = nms(std::move(raw), nms_threshold_);
    faces = std::move(kept);
    return true;
}

bool InProcessFaceRecognizer::extractFeature(const cv::Mat& bgr,
                                             const InProcessFaceBox& face,
                                             std::vector<float>& feature_vec,
                                             float& raw_l2_norm) {
    cv::Mat aligned;
    if (!align_face(bgr, face.landmarks, aligned)) return false;

    cv::Mat rgb;
    cv::cvtColor(aligned, rgb, cv::COLOR_BGR2RGB);

    std::vector<float> input_f32(REC_SIZE * REC_SIZE * 3);
    const float norm_scale = 1.0f / 127.5f;
    const size_t total = rgb.total() * rgb.channels();
    for (size_t i = 0; i < total; ++i) {
        input_f32[i] = ((float)rgb.data[i] - 127.5f) * norm_scale;
    }

    rknn_input input{};
    input.index = 0;
    input.type = RKNN_TENSOR_FLOAT32;
    input.size = (uint32_t)(input_f32.size() * sizeof(float));
    input.fmt = RKNN_TENSOR_NHWC;
    input.buf = input_f32.data();
    input.pass_through = 0;

    if (rknn_inputs_set(rec_ctx_, 1, &input) < 0) return false;
    if (rknn_run(rec_ctx_, nullptr) < 0) return false;

    rknn_output output{};
    output.want_float = 1;
    if (rknn_outputs_get(rec_ctx_, 1, &output, nullptr) < 0) return false;

    const float* feature = (const float*)output.buf;
    feature_vec.assign(feature, feature + FEATURE_DIM);
    double sum = 0.0;
    for (int i = 0; i < FEATURE_DIM; ++i) sum += (double)feature[i] * (double)feature[i];
    raw_l2_norm = (float)std::sqrt(sum);
    l2_normalize(feature_vec.data(), FEATURE_DIM);
    rknn_outputs_release(rec_ctx_, 1, &output);
    return true;
}

FaceDetectionResult InProcessFaceRecognizer::detectAndExtract(const std::string& image_path) {
    FaceDetectionResult result;
    if (!isInitialized()) {
        std::cerr << "InProcessFaceRecognizer: not initialized" << std::endl;
        return result;
    }

    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        std::cerr << "InProcessFaceRecognizer: failed to read image: " << image_path << std::endl;
        return result;
    }

    std::vector<InProcessFaceBox> boxes;
    if (!detectFaces(bgr, boxes)) {
        std::cerr << "InProcessFaceRecognizer: detection failed" << std::endl;
        return result;
    }

    result.image_path = image_path;
    result.image_size = cv::Size(bgr.cols, bgr.rows);
    result.feature_dim = FEATURE_DIM;
    result.feature_normalized = true;

    int face_id = 0;
    for (const auto& f : boxes) {
        // clamp 后可能退化为空框；旧链路在 JSON 解析层就剔除非法 bbox，此处同语义
        if (f.x2 <= f.x1 || f.y2 <= f.y1) continue;

        std::vector<float> feature;
        float raw_l2 = 0.0f;
        if (!extractFeature(bgr, f, feature, raw_l2)) {
            // 单脸失败只剔除该脸（旧 exe 是整图失败，此处放宽——退化脸多为
            // 贴边小脸，丢弃比阻断整批点名更合理，与非法 bbox 剔除同一粒度）
            std::cerr << "InProcessFaceRecognizer: feature extraction failed for a face" << std::endl;
            continue;
        }

        DetectedFace face;
        face.face_id = face_id++;
        face.score = f.score;
        face.bbox = cv::Rect((int)f.x1, (int)f.y1,
                             (int)(f.x2 - f.x1), (int)(f.y2 - f.y1));
        face.landmarks.clear();
        for (int k = 0; k < 5; ++k)
            face.landmarks.push_back(cv::Point2f(f.landmarks[k][0], f.landmarks[k][1]));
        face.raw_l2_norm = raw_l2;
        face.feature = std::move(feature);
        result.faces.push_back(std::move(face));
    }
    result.num_faces = (int)result.faces.size();
    return result;
}
