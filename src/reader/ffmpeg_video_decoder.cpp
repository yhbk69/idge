// video_decoder.cpp
//
// ============================================================================
// 硬件解码完整流程说明（Rockchip RK3588 平台）
// ============================================================================
//
// 本文件实现了从视频流接收 → 硬件解码 → 色彩空间转换 → 模型推理的完整流水线。
// 核心目标：利用 Rockchip MPP (Media Process Platform) 硬件加速解码，
//           避免 CPU 参与视频解码，释放 CPU 算力给其他任务。
//
// 【整体数据流】
//
//   视频流(RTSP/文件)
//        │
//        ▼
//   ┌─────────────────────────────────────────────┐
//   │  1. FFmpeg demuxer (av_read_frame)          │
//   │     读取压缩的 H.264/H.265 数据包 (packet) │
//   └─────────────────────────────────────────────┘
//        │
//        ▼
//   ┌─────────────────────────────────────────────┐
//   │  2. MPP 硬件解码器 (h264_rkmpp/hevc_rkmpp) │
//   │     将压缩数据解码为 NV12 格式的视频帧      │
//   │     输出：DRM PRIME fd (DMA-BUF)            │
//   │     注意：解码在 GPU/VPU 上完成，不经 CPU    │
//   └─────────────────────────────────────────────┘
//        │
//        ▼
//   ┌─────────────────────────────────────────────┐
//   │  3. RGA 色彩转换 (NV12 → RGBA)              │
//   │     Rockchip RGA 硬件加速，零拷贝(fd→fd)    │
//   └─────────────────────────────────────────────┘
//        │
//        ▼
//   ┌─────────────────────────────────────────────┐
//   │  4. RGA 缩放+Letterbox (RGBA → RGB 640×640) │
//   │     保持宽高比，居中放置，填充灰色背景       │
//   │     输出送入 RKNN NPU 进行目标检测推理       │
//   └─────────────────────────────────────────────┘
//
// 【关键概念解释】
//
//   DRM PRIME fd / DMA-BUF：
//     - Linux 内核提供的零拷贝缓冲区共享机制
//     - 一个文件描述符(fd)指向一块物理内存，可以同时被多个硬件访问
//     - 解码器输出的帧数据直接通过 fd 传递给 RGA，无需 CPU 拷贝
//     - 类似于显卡显存的概念，但更通用
//
//   MPP (Media Process Platform)：
//     - Rockchip 官方的多媒体处理框架
//     - 封装了 VPU (Video Processing Unit) 的硬件能力
//     - 通过 FFmpeg 的 rkmpp 后端调用，对用户透明
//
//   RGA (Rocket Graphics Acceleration)：
//     - Rockchip 的 2D 图形加速引擎
//     - 专用于图像缩放、色彩转换、旋转等操作
//     - 比 CPU 处理快 10-50 倍，功耗更低
//
//   NV12 格式：
//     - YUV 4:2:0 平面格式，视频解码的标准输出格式
//     - Y 平面（亮度）+ UV 交错平面（色度）
//     - 内存布局：Y(w×h) + UV(w×h/2) = 1.5 × 宽 × 高 字节
//
//   Letterbox：
//     - 保持原始视频宽高比，缩放到目标尺寸
//     - 不匹配的区域用灰色(114,114,114)填充
//     - 这是 YOLO 系列模型的标准预处理方式
//
// 【代码审查视角的已知注意事项】（详见各处内联注释）：
//   - 资源释放配对：fmt_ctx/dec_ctx/frame/pkt/dst_bufs 在正常退出路径统一释放；
//     中途的多个 emit error + return 分支需注意 avformat_open_input 失败时
//     FFmpeg 已自行释放 fmt_ctx（不能再 close），属"看似缺清理、实则正确"的点。
//   - results.time 单位隐患：image->time 取 system_clock 的 count()（Linux 上为
//     纳秒），而 common.hpp 注释标为"毫秒"，且 alarm_manager.h 限流常量按纳秒
//     比较——三者必须统一口径，否则限流窗口失真（详见 common.hpp 警示注释）。
//   - 双缓冲切换仅在 RGA 转换成功路径发生（back_buf = 1 - back_buf），失败帧
//     复用同一缓冲不会造成读写竞争（该帧根本没有输出）。
//
// ============================================================================

#include "ffmpeg_video_decoder.h"
#include "rga_converter.h"
#include "../model_repo/model_registry.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <cerrno>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include "DmaFrameBuffer.h"
#include "image_utils.h"
#include "ThreadPool.hpp"
#include "helmet_task.h"
#include "alarm_manager.h"
#include "fence_manager.h"
#include "fence_checker.h"
// X11 头文件把 None/Null/Bool 等定义成宏(0/int)，会顶掉 Qt 头文件里同名的
// 枚举成员（如 QUrl::None、QJsonValue::Null），这里统一去掉这些宏避免解析报错
#undef None
#undef Null
#undef Bool
#include "ConfigManager.h"
#include <fstream>
#include <arm_neon.h>   // ARM NEON SIMD 加速

// ============================================================
// 报警截图异步写入器
//
// 为什么用后台线程：PNG/JPEG 编码比较费 CPU，若在解码线程同步执行会拖低帧率，
// 所以解码线程只负责【拷贝像素 + 提交】，编码写盘在独立后台线程完成。
//
// 为什么用 JPG 不用 PNG：
//   PNG 的 zlib 压缩在板子上非常慢；JPG(turbojpeg) 快得多、文件也小。
//   image_utils 的 jpg 写入只支持 RGB(3通道)，所以先把 RGBA 转成 RGB 再写。
//
// 目录：alarms/日期/通道N_类别_时间.jpg
// ============================================================
namespace {

// 一张待写盘的任务：存一份 RGB 像素拷贝（去掉 alpha，体积比 RGBA 小 25%）+ 路径
struct SnapJob {
    std::vector<unsigned char> rgb;    // RGB 像素拷贝（w*h*3 字节）
    int w = 0;
    int h = 0;
    QString path;                      // 保存路径(.jpg)
};

class SnapWriter {
public:
    static SnapWriter &get()
    {
        static SnapWriter w;            // 进程内唯一写盘线程，首次提交时启动
        return w;
    }

    // 提交一张截图：RGBA 像素先转成 RGB 再入队，立即返回（不阻塞解码线程）
    // 队列满（积压太多）时丢弃最旧的一张，避免内存无限增长
    void submit(int w, int h, const unsigned char *rgba, const QString &path)
    {
        SnapJob job;
        job.w = w;
        job.h = h;
        job.path = path;
        job.rgb.resize((size_t)w * h * 3);

        // RGBA(4字节/像素) -> RGB(3字节/像素)，跳过 alpha 字节
        // 使用 ARM NEON SIMD 加速，每次处理 8 个像素
        const unsigned char *s = rgba;
        unsigned char *d = job.rgb.data();
        size_t n = (size_t)w * h;
        size_t i = 0;

        // NEON 优化：每次处理 8 个 RGBA 像素 -> 8 个 RGB 像素
        // vld4_u8 加载 32 字节（8×4），交错解包为 4 个 8×8 通道
        for (; i + 8 <= n; i += 8) {
            uint8x8x4_t rgba8 = vld4_u8(s);   // 加载 8 个像素的 R,G,B,A
            vst3_u8(d, *(uint8x8x3_t*)&rgba8); // 存储 8 个像素的 R,G,B（丢弃 A）
            s += 32;  // 8 像素 × 4 字节
            d += 24;  // 8 像素 × 3 字节
        }

        // 处理剩余不足 8 个的像素
        for (; i < n; ++i) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            s += 4;
            d += 3;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            if ((int)q_.size() >= kMaxPending) q_.pop();   // 满了丢最旧
            q_.push(std::move(job));
        }
        cv_.notify_one();
    }

private:
    static const int kMaxPending = 4;   // 最多积压 4 张，防止内存暴涨

    SnapWriter() { th_ = std::thread([this] { run(); }); }
    ~SnapWriter()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();   // 退出前把队列里剩余的都写完
    }

    void run()
    {
        for (;;) {
            SnapJob job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                job = std::move(q_.front());
                q_.pop();
            }
            writeJpg(job);
        }
    }

    // 真正把 RGB 像素编码成 JPG 并写盘（只在后台线程调用）
    static void writeJpg(SnapJob &job)
    {
        image_buffer_t img;
        memset(&img, 0, sizeof(img));
        img.format = image_format_t::IMAGE_FORMAT_RGB888;
        img.virt_addr = job.rgb.data();   // 指向 RGB 像素拷贝
        img.width = job.w;
        img.height = job.h;
        img.width_stride = job.w;
        write_image(job.path.toUtf8().constData(), &img);
    }

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<SnapJob> q_;
    bool stop_ = false;
};

} // namespace

// 生成截图路径并提交给后台线程，返回保存路径
static QString submitAlarmSnapshot(int channel, const QString &className,
                                   int w, int h, const unsigned char *rgba)
{
    // 一级目录 alarms/ 存截图根目录
    if (::mkdir("alarms", 0755) != 0 && errno != EEXIST) {
        return QString();
    }
    // 二级目录 alarms/yyyyMMdd 按天归档
    QString day = QDate::currentDate().toString("yyyyMMdd");
    QString dir = QString("alarms/%1").arg(day);
    if (::mkdir(dir.toUtf8().constData(), 0755) != 0 && errno != EEXIST) {
        return QString();
    }

    // 文件名：通道N_类别_时分秒_毫秒.jpg
    QString path = QString("%1/ch%2_%3_%4.jpg")
        .arg(dir)
        .arg(channel + 1)
        .arg(className)
        .arg(QDateTime::currentDateTime().toString("HHmmss_zzz"));

    // 提交给后台线程写盘（拷贝像素副本），解码线程立即返回不等待
    SnapWriter::get().submit(w, h, rgba, path);
    return path;
}

// ============================================================================
// 硬件像素格式回调函数
// ============================================================================
// FFmpeg 解码器初始化时会询问："你支持什么输出格式？"
// 这个回调告诉 FFmpeg："我需要 DRM_PRIME 格式"（即 DMA-BUF fd）
//
// 为什么选 DRM_PRIME 而不是 NV12？：
//   - NV12：解码器将数据写入内存，CPU 需要访问才能处理 → 慢
//   - DRM_PRIME：解码器输出 DMA-BUF fd，RGA 直接读取同一块物理内存 → 零拷贝
//
// 调用时机：avcodec_open2() 内部，解码器初始化阶段
// 返回值：AV_PIX_FMT_DRM_PRIME（告诉解码器用 DMA-BUF 输出）
// ============================================================================
enum AVPixelFormat GetHWFormat(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    // 遍历解码器支持的像素格式列表，优先选择 DRM_PRIME
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;  // 找到 DMA-BUF 格式，直接返回
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;  // 没有找到硬件格式，解码器会退回到 CPU 处理
}

// ============================================================================
// FFmpeg 中断回调函数
// ============================================================================
// 当 FFmpeg 执行阻塞操作（如等待网络数据）时，会定期调用此回调。
// 返回 0 = 继续执行，返回 1 = 中断当前操作（返回 AVERROR_EXIT）
//
// 使用场景：
//   - 用户调用 stop() 将 running_ 设为 false
//   - FFmpeg 在 av_read_frame() 或 avcodec_receive_frame() 中阻塞
//   - 此回调检测到 running_=false，立即中断 FFmpeg 的阻塞操作
//   - 解码线程快速退出，避免 UI 线程卡死
//
// 为什么需要中断机制？：
//   - RTSP 流可能长时间无数据（网络断开、摄像头重启等）
//   - 如果没有中断机制，stop() 会一直等待 FFmpeg 返回
//   - 用户点击"停止"后界面会卡住，体验极差
// ============================================================================
static int decodeInterruptCallback(void *ctx)
{
    std::atomic<bool> *running = static_cast<std::atomic<bool> *>(ctx);
    return running->load() ? 0 : 1;  // 0=继续, 1=中断
}

// ============================================================================
// 构造函数
// ============================================================================
// 初始化 DMA 缓冲池和级联推理任务
//
// DMA 缓冲池参数说明：
//   - capacity=64: 预分配 64 个缓冲区（应对多通道并发）
//   - 640×640: YOLO 模型的标准输入尺寸
//   - RK_FORMAT_BGR_888: RGB 三通道，每通道 8 位
//   - align=16: 内存对齐到 16 字节（RGA 硬件要求）
//
// 为什么需要缓冲池？：
//   - 避免频繁 malloc/free 带来的内存碎片和性能开销
//   - 多个通道共享同一个缓冲池，内存使用可控
//   - 引用计数自动管理生命周期，无需手动释放
// ============================================================================
FFmpegVideoDecoder::FFmpegVideoDecoder(QObject *parent) : QObject(parent) 
{
    // 创建 DMA 缓冲池：64 个 640×640 的 RGB 缓冲区
    dmaBufferPool_ = std::make_shared<DmaBufferPool>(64, 640, 640, RK_FORMAT_BGR_888, 16);

    // 从 config.json 读取模型路径，构建级联推理任务列表
    // 每个模型会创建一个独立的 PpeTask（推理线程）
    buildCascadeTasks();
}

// ============================================================
// 级联任务构建：
//   由 ModelRegistry::resolveCascadeSlots() 解析 5 个槽位
//   （槽位 1 = 全局 model.path，2~5 = cascade.models；每槽自带标签）。
//   每个启用槽位建一个 PpeTask（独立推理线程）+ 一个结果队列，
//   NPU 核心 0/1/2 轮流分配；taskConfig.result_id = 紧凑任务下标，
//   推理结果经 od.id 路由到 AlarmManager 的按槽类名表。
//   空路径 = 不启用该槽（界面里空着即可）。
// ============================================================
static std::vector<std::string> readLabelNames(const QString &labelPath)
{
    std::vector<std::string> names;
    std::ifstream infile(labelPath.toStdString());
    std::string line;
    while (infile && std::getline(infile, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.push_back(line);
    }
    return names;
}

void FFmpegVideoDecoder::buildCascadeTasks()
{
    ConfigManager &cfg = ConfigManager::instance();
    // 解码器构造比主窗口 ConfigManager::load() 早，这里确保配置已读入
    if (cfg.modelPath().isEmpty() && cfg.labelPath().isEmpty()) {
        cfg.load("config.json");
    }

    // 槽位解析：模型路径 + 每槽专属标签（库内 labels.txt 优先于全局 label）
    QList<SlotModel> slotList = ModelRegistry::resolveCascadeSlots();
    bool anyEnabled = false;
    for (const SlotModel &s : slotList) {
        if (!s.modelPath.trimmed().isEmpty()) { anyEnabled = true; break; }
    }
    if (!anyEnabled) {
        // 兜底：什么也没配就用模型库默认模型
        SlotModel fb;
        fb.modelPath = "model/library/yolo11n-coco/model.rknn";
        fb.labelPath = cfg.labelPath().isEmpty()
                           ? QStringLiteral("model/library/yolo11n-coco/labels.txt")
                           : cfg.labelPath();
        slotList.clear();
        slotList.append(fb);
    }

    // 每个启用槽位一个推理任务 + 一个结果队列 + 一份类名表
    for (const SlotModel &s : slotList) {
        QString mp = s.modelPath.trimmed();
        if (mp.isEmpty())
            continue;
        QString lp = s.labelPath.trimmed();
        if (lp.isEmpty()) lp = "model/library/yolo11n-coco/labels.txt";

        const int slotIdx = (int)tasks_.size();  // 紧凑下标，即 result_id

        TaskConfig taskConfig;
        taskConfig.core_mask = (rknn_core_mask)(RKNN_NPU_CORE_0 + (slotIdx % 3)); // 0/1/2 轮流
        taskConfig.modelPath = mp.toStdString();
        taskConfig.labelPath = lp.toStdString();
        taskConfig.result_id = slotIdx;

        PpeTask *task = new PpeTask(taskConfig);
        tasks_.push_back(task);

        // 每个任务一个独立结果队列（容量 8，满了丢旧保新）
        slotQueues_.push_back(std::make_shared<PriorityQueue<object_detect_result_list>>(8));

        // 类名表：画框用 std 版，报警侧用 QString 版（同一数据源，同下标）
        slotClassNames_.push_back(readLabelNames(lp));
        QStringList qnames;
        for (const auto &n : slotClassNames_.back()) qnames << QString::fromStdString(n);
        AlarmManager::instance().setSlotClassNames(slotIdx, qnames);
    }

    qDebug() << "Cascade tasks:" << tasks_.size();
    for (size_t k = 0; k < tasks_.size(); ++k) {
        qDebug() << "  slot" << k + 1 << "->" << QString::fromStdString(tasks_[k]->getModelPath())
                 << "labels:" << (int)slotClassNames_[k].size();
    }
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { stop(); }

// ============================================================================
// 启动解码器
// ============================================================================
// 启动流程：
//   1. 停止之前的解码线程（如果正在运行）
//   2. 启动所有级联推理任务（每个模型一个线程）
//   3. 创建新的 Qt 线程运行 decodeLoop()
//
// 为什么用 Qt 线程？：
//   - decodeLoop() 需要 emit frameReady() 信号
//   - Qt 信号/槽默认在接收者所在线程执行
//   - UI 线程需要接收 frameReady() 来更新画面
//   - 用 Qt 线程可以自动处理跨线程信号传递
// ============================================================================
void FFmpegVideoDecoder::start(const QString &url)
{
    if (running_)
        stop();

    url_ = url;
    running_ = true;

    // 启动所有级联推理任务（每个模型一个线程）
    for (PpeTask *task : tasks_) {
        task->start();
    }

    // 创建 Qt 线程，在线程中运行 decodeLoop()
    thread_ = new QThread;
    moveToThread(thread_);
    connect(thread_, &QThread::started, this, &FFmpegVideoDecoder::decodeLoop);
    connect(thread_, &QThread::finished, thread_, &QThread::deleteLater);

    thread_->start();
}

// ============================================================================
// 停止解码器
// ============================================================================
// 停止顺序（有讲究）：
//   1. 设置 running_ = false → 触发中断回调，FFmpeg 立即返回
//   2. 停止推理任务 → 通知推理线程退出（不阻塞）
//   3. 停止解码线程 → 等待 decodeLoop() 退出
//   4. 回收推理线程 → 有界等待（1.5s），超时 detach
//
// 为什么先停推理再停解码？：
//   - 推理线程可能在等待队列数据
//   - 如果先停解码，推理线程会一直阻塞
//   - 先停推理，队列关闭后解码线程的 put() 会快速失败
//
// 为什么 stopBestEffort 有超时？：
//   - NPU 忙时，推理线程可能无法响应 stop 请求
//   - 如果无限等待，主线程会卡死
//   - detach 虽然不优雅，但保证了系统响应性
// ============================================================================
void FFmpegVideoDecoder::stop()
{
    running_ = false;

    // 1) 先请求所有推理任务退出（关队列唤醒，不阻塞）
    for (PpeTask *task : tasks_) {
        task->requestStop();
    }

    // 2) 停解码线程（interrupt_callback 中断 av_read_frame，处理完当前帧即退出）
    if (thread_)
    {
        thread_->quit();
        if (!thread_->wait(3000)) {
            qWarning() << "Decoder thread did not stop in time, terminating";
            thread_->terminate();
            thread_->wait(1500);
        }
        thread_ = nullptr;
    }

    // 3) 有界回收推理线程：最多等 1.5s，超时 detach，绝不阻塞 UI 线程
    //    审查点：tasks_ 只 stop 从不 delete——析构函数同样只调 stop()。
    //    这是有意为之：detach 后的推理线程仍可能访问 this/队列，delete 会
    //    造成 use-after-free；代价是每路解码器泄漏至多 5 个 PpeTask 对象
    //    （通道数固定、进程生命周期内不重复创建，总量可控）。
    //    末尾把 running_ 复位为 true：此时线程已停，标志仅供下一轮 start()
    //    的 interrupt 回调使用，不代表"正在运行"。
    for (PpeTask *task : tasks_) {
        task->stopBestEffort(1500);
    }

    running_ = true;  // 重置，为下次 start() 准备
}

// ============================================================================
// reloadAllModels: 热更新所有模型
// ============================================================================
// 在不停止解码的情况下替换所有推理模型。
// 用于生产环境中更换检测模型（如从安全帽检测切换到反光背心检测）。
//
// 流程：
//   1. 逐个调用 PpeTask::reloadModel() 替换模型
//   2. 更新类别名称列表
//   3. 返回成功替换的数量
//
// 注意：
//   - 解码线程继续运行，但推理会短暂暂停（~100ms/模型）
//   - 新模型加载失败：旧模型原样重启，保持不变
//   - 推理线程未在时限内退出：放弃该槽位热更新（旧线程随后自行停止，
//     可稍后再次触发重试；期间该槽位不产结果但不崩溃、不丢 NPU 上下文）
// ============================================================================
int FFmpegVideoDecoder::reloadAllModels(const QStringList &modelPaths, const QStringList &labelPaths)
{
    int successCount = 0;
    int taskIndex = 0;

    for (int i = 0; i < modelPaths.size() && taskIndex < tasks_.size(); i++) {
        QString path = modelPaths[i];
        if (path.isEmpty()) {
            continue;  // 跳过空路径
        }

        QString labelPath = (i < labelPaths.size()) ? labelPaths[i] : labelPaths[0];

        PpeTask *task = tasks_[taskIndex];
        // 热更新只换模型、不改核分配：沿用 buildCascadeTasks 为该槽位定的核掩码
        bool ok = task->reloadModel(
            path.toStdString(),
            labelPath.toStdString(),
            task->currentCoreMask());

        if (ok) {
            successCount++;
            qInfo() << "Model" << i << "reloaded:" << path;

            // 同步该槽位的类名表（画框 + 报警侧都要换成新模型的标签）
            if (taskIndex < (int)slotClassNames_.size()) {
                slotClassNames_[taskIndex] = readLabelNames(labelPath);
                QStringList qnames;
                for (const auto &n : slotClassNames_[taskIndex])
                    qnames << QString::fromStdString(n);
                AlarmManager::instance().setSlotClassNames(taskIndex, qnames);
            }
        } else {
            qWarning() << "Model" << i << "reload failed:" << path;
        }

        taskIndex++;
    }

    return successCount;
}

// ============================================================================
// setVideoRecordingEnabled: 启用/禁用视频录制
// ============================================================================
void FFmpegVideoDecoder::setVideoRecordingEnabled(bool enabled, int bufferSeconds)
{
    if (enabled && !videoRecorder_) {
        videoRecorder_ = new VideoRecorder(bufferSeconds, 25, this);
        videoRecordingEnabled_ = true;
        qInfo() << "Video recording enabled, buffer:" << bufferSeconds << "s";
    } else if (!enabled && videoRecorder_) {
        delete videoRecorder_;
        videoRecorder_ = nullptr;
        videoRecordingEnabled_ = false;
        qInfo() << "Video recording disabled";
    }
}

// ============================================================================
// dumpVideoToFile: 将环形缓冲区 dump 到 MP4 文件
// ============================================================================
bool FFmpegVideoDecoder::dumpVideoToFile(const QString &filePath)
{
    if (!videoRecorder_ || !videoRecorder_->isInitialized()) {
        qWarning() << "Video recorder not initialized";
        return false;
    }
    return videoRecorder_->dumpToFile(filePath);
}

// ============================================================================
// 主解码循环
// ============================================================================
// 这是整个硬件解码流程的核心函数，运行在独立的 Qt 线程中。
// 执行流程：
//   1. 打开视频流（支持 RTSP/本地文件）
//   2. 查找并初始化 Rockchip MPP 硬件解码器
//   3. 分配 DMA-BUF 输出缓冲区
//   4. 循环读取压缩数据包 → 送入解码器 → 获取解码帧 → RGA转换 → 送入推理
//
// 线程安全：
//   - running_ 是 atomic<bool>，可在任意线程安全读写
//   - decodeInterruptCallback 在 IO 阻塞时检查 running_，实现优雅退出
// ============================================================================
void FFmpegVideoDecoder::decodeLoop()
{
    int ret;

    // ===== 步骤1: 打开视频流 =====
    // AVFormatContext 是 FFmpeg 的核心结构，管理输入流的所有信息
    AVFormatContext *fmt_ctx = nullptr;

    // 设置中断回调：当用户调用 stop() 将 running_ 设为 false 时，
    // av_read_frame() 和 avcodec_receive_frame() 会立即返回错误，
    // 使解码线程能快速退出，避免阻塞在 IO 上（尤其是网络流）
    AVIOInterruptCB interrupt_cb = {decodeInterruptCallback, &running_};
    fmt_ctx = avformat_alloc_context();
    fmt_ctx->interrupt_callback = interrupt_cb;

    // 打开输入流：可以是本地文件(如 video.mp4)或网络流(如 rtsp://...)
    // 这一步会自动探测容器格式（MP4/RTSP/FLV等）和编解码器信息
    ret = avformat_open_input(&fmt_ctx, url_.toUtf8().constData(), nullptr, nullptr);
    if (ret < 0)
    {
        avformat_free_context(fmt_ctx);
        fmt_ctx = nullptr;
        // 审查点：FFmpeg 约定 avformat_open_input 失败时内部已 free 并把传入指针
        // 置 NULL，此处 free 实为对 NULL 的安全空操作，保留仅作防御性写法。
        emit error("Cannot open file");
        return;
    }

    // 读取流的详细信息（编码参数、帧率、分辨率等）
    // 对于网络流可能需要几秒钟来探测
    // 审查点：返回值未检查。RTSP 断流/被 interrupt 打断时可能拿到不完整的
    // codecpar，后续 av_find_best_stream 会因找不到视频流而走错误分支兜底。
    avformat_find_stream_info(fmt_ctx, nullptr);

    // 在所有流中找到最佳的视频流（可能是多个视频流，选质量最高的）
    // vidx 是视频流在 fmt_ctx->streams[] 数组中的索引
    int vidx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0)
    {
        emit error("No video stream");
        avformat_close_input(&fmt_ctx);
        return;
    }

    // ===== 步骤2: 初始化 Rockchip MPP 硬件解码器 =====
    //
    // FFmpeg 的解码器名称约定：
    //   - "h264_rkmpp"  → H.264 硬件解码（Rockchip MPP 后端）
    //   - "hevc_rkmpp"  → H.265/HEVC 硬件解码（Rockchip MPP 后端）
    //   - "h264" / "hevc" → FFmpeg 内置软解码（CPU，慢）
    //
    // 搜索顺序：优先用硬件解码，找不到就退回到软解
    const AVCodec *codec = avcodec_find_decoder_by_name("h264_rkmpp");
    if (!codec)
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
    if (!codec)
        codec = avcodec_find_decoder(fmt_ctx->streams[vidx]->codecpar->codec_id);  // 软解兜底
    if (!codec)
    {
        emit error("No decoder");
        avformat_close_input(&fmt_ctx);
        return;
    }

    // 创建解码器上下文，用于存放解码器的状态和配置
    AVCodecContext *dec_ctx = avcodec_alloc_context3(codec);

    // 将视频流的编码参数（分辨率、profile、level等）拷贝到解码器上下文
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[vidx]->codecpar);

    // 设置硬件像素格式回调：告诉解码器"我需要 DRM_PRIME (DMA-BUF) 输出"
    // 这个回调在 avcodec_open2() 内部被调用
    enum AVPixelFormat stHWPixFmt = AV_PIX_FMT_DRM_PRIME;
    dec_ctx->get_format = GetHWFormat;

    // ===== 创建硬件设备上下文 (rkmpp) =====
    // 这一步初始化 Rockchip MPP 硬件加速器
    // type = "rkmpp" 对应 Rockchip 的 MPP (Media Process Platform)
    // 创建后，解码器就知道用哪个硬件设备进行解码
    AVBufferRef *pHWDeviceCtx;

    enum AVHWDeviceType type;
    type = av_hwdevice_find_type_by_name("rkmpp");

    int err = 0;
    if ((err = av_hwdevice_ctx_create(&pHWDeviceCtx, type,
									  NULL, NULL, 0)) < 0) {
		qWarning() << "Failed to create DRM HW device context";
        char errrbuf[256];
        av_strerror(err, errrbuf, sizeof(errrbuf));
        printf("failed %s\n", errrbuf);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
		return ;
	}

    // 将硬件设备上下文绑定到解码器上下文
    // 之后 avcodec_open2() 会使用这个硬件设备进行解码
    dec_ctx->hw_device_ctx = av_buffer_ref(pHWDeviceCtx);

    // thread_count=1: 硬件解码器本身是单线程的，多线程反而会增加开销
    dec_ctx->thread_count = 1;

    // 打开解码器：此时解码器会初始化 MPP 硬件
    // 如果硬件解码器初始化失败，FFmpeg 会尝试软解
    // 审查点：返回值未检查；若打开失败，后续 receive_frame 恒为错误，
    // 表现为"无画面但不崩溃"，排查时可先确认此步日志。
    avcodec_open2(dec_ctx, codec, nullptr);

    // 从解码器上下文获取视频分辨率（可能与流信息中的略有不同）
    int vid_w = dec_ctx->width;
    int vid_h = dec_ctx->height;
    qDebug() << "Decoder:" << codec->name << vid_w << "x" << vid_h;

    // 初始化视频录制器（如果已启用）
    if (videoRecordingEnabled_ && videoRecorder_) {
        videoRecorder_->init(vid_w, vid_h);
    }

    emit statusChanged(channel_, 1);

    // ===== 步骤3: 分配 DMA-BUF 输出缓冲区 =====
    //
    // 数据流：MPP解码(NV12 fd) → RGA转换 → RGBA缓冲区 → RGA缩放 → RGB缓冲区
    //
    // 这里分配 RGBA 缓冲区，用于存放 RGA 色彩转换的结果
    // 使用双缓冲（两个 buffer 交替使用），避免 RGA 还在读 A 时解码器就开始写 A
    //
    // 为什么用 DMA-BUF 而不是普通内存？：
    //   - DMA-BUF 可以被多个硬件（MPP、RGA、NPU）同时访问
    //   - 无需 CPU 介入拷贝数据，直接通过 fd 在硬件间传递
    //   - 普通内存需要 CPU 来回拷贝，效率极低

    // 打开 DRM 设备，用于分配 DMA-BUF（显存类型的内存）
    // 审查点：两次 open 都失败时 drm_fd<0，循环出口的 close(drm_fd) 对负数
    // fd 只会返回 EBADF，无副作用；DMA 分配失败会在 DmaFrameBuffer::alloc 处暴露。
    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
        drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);

    // 双缓冲 RGBA（RGA 输出 → EGLImage 输入）
    // back_buf 在 0/1 之间交替切换，实现流水线并行
    // 审查点：rgba_bufs 为早期手动管理缓冲的遗留声明，实际未被使用
    // （现由下方 new 出的 DmaFrameBuffer 承担），保留仅为兼容原代码形态。
    DmaBuffer rgba_bufs[2];


    int back_buf = 0;

    // 使用 DmaFrameBuffer 管理 DMA-BUF 的分配和释放
    // DRM_FORMAT_RGBA8888: 每像素 4 字节（R/G/B/A 各 8 位）
    DmaFrameBuffer* dst_bufs[2];
    dst_bufs[0] = new DmaFrameBuffer();
    dst_bufs[0]->setWidth(vid_w);
    dst_bufs[0]->setHeight(vid_h);
    dst_bufs[0]->setFormat(DRM_FORMAT_RGBA8888);
    dst_bufs[0]->alloc();
    
    dst_bufs[1] = new DmaFrameBuffer();
    dst_bufs[1]->setWidth(vid_w);
    dst_bufs[1]->setHeight(vid_h);
    dst_bufs[1]->setFormat(DRM_FORMAT_RGBA8888);
    dst_bufs[1]->alloc();


    // ===== 步骤4: 主解码循环 =====
    //
    // 每次循环处理一个视频帧：
    //   av_read_frame → avcodec_send_packet → avcodec_receive_frame → RGA转换 → 推理
    //
    // AVFrame: 存放一帧解码后的视频数据
    //   - frame->format: 像素格式（这里应该是 AV_PIX_FMT_DRM_PRIME）
    //   - frame->data[0]: 指向 AVDRMFrameDescriptor，包含 DMA-BUF fd 信息
    //   - frame->width / frame->height: 视频分辨率
    //
    // AVPacket: 存放一帧压缩的视频数据
    //   - pkt->data: 压缩数据的指针
    //   - pkt->size: 压缩数据的大小
    //   - pkt->stream_index: 属于哪个流（音频/视频/字幕）
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    int frame_count = 0;

    QElapsedTimer timer;
    timer.start();
    timer.elapsed();
    TIMER t;

    while (running_)
    {
        // ===== 4.1 读取一个压缩数据包 =====
        // av_read_frame() 从容器中读取下一个 packet
        // 对于 RTSP 流，这里可能会阻塞等待网络数据
        // 返回 0 表示成功，AVERROR_EOF 表示流结束
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
        {
            // 流已结束（文件播放完毕或连接断开）
            // 发送 NULL packet 通知解码器"没有更多数据了，输出剩余帧"
            // 审查点：drain 出的尾帧只做 unref 直接丢弃，并未走下方完整的
            // RGA 转换/画框/emit 流程（与注释"同下方逻辑"不符）——文件播放
            // 结束时最后若干解码帧不上屏，属可接受的收尾取舍。
            // 另注意：若 ret 是 EAGAIN/被 interrupt 打断（非 EOF），同样会
            // 提前 break 走清理路径，stop() 时表现为"立即收尾"。
            avcodec_send_packet(dec_ctx, nullptr);
            while (running_ && avcodec_receive_frame(dec_ctx, frame) == 0)
            {
                // 处理剩余帧（同下方逻辑）
                av_frame_unref(frame);
            }
            break;
        }

        // 只处理视频流，跳过音频/字幕流
        if (pkt->stream_index != vidx)
        {
            av_packet_unref(pkt);
            continue;
        }

        // ===== 4.2 将压缩数据包送入解码器 =====
        // avcodec_send_packet() 将压缩数据送入解码器
        // 解码器内部会将数据放入队列，异步处理
        // MPP 硬件会自动接管解码工作，CPU 几乎不参与
        // 审查点：send_packet 返回值未检查，随后无条件 av_packet_unref。
        // 严格 API 用法是 EAGAIN 时应先 receive 再重试 send；这里等价于
        // "解码器满时直接丢这一包"，对实时监控是合理降级（宁可丢帧不堆积）。
        // packet 引用由 unref 归零释放，与 4.1 分配配对；下面 stream_index
        // 不匹配分支同样先 unref 再 continue，无泄漏路径。
        avcodec_send_packet(dec_ctx, pkt);
        av_packet_unref(pkt);  // 立即释放 packet，减少内存占用

        // ===== 4.3 从解码器获取解码后的帧 =====
        // avcodec_receive_frame() 从解码器获取一帧解码后的数据
        // 一次 send 可能产生多帧（B帧重排）或0帧（还在缓冲）
        // 返回 0 表示成功获取一帧，AVERROR(EAGAIN) 表示需要更多输入
        while (running_ && avcodec_receive_frame(dec_ctx, frame) == 0)
        {
            t.tik();  // 计时开始
            frame_count++;

            // ===== 4.4 从解码帧中提取 DMA-BUF fd =====
            //
            // 解码后的帧格式是 AV_PIX_FMT_DRM_PRIME（不是 NV12！）
            // DRM_PRIME 是一种"引用"，通过 fd 指向实际的物理内存
            //
            // AVDRMFrameDescriptor 结构：
            //   - objects[]: DMA-BUF 对象数组（通常只有一个）
            //     - objects[0].fd: DMA-BUF 的文件描述符（用于 RGA 读取）
            //   - layers[]: 层信息（NV12 通常只有一个 YUV 层）
            //     - layers[0].planes[0].pitch: 每行的字节数（可能因对齐而大于 width）
            //
            // 为什么需要 stride？：
            //   硬件要求内存按 16/32/64 字节对齐，所以实际内存宽度 >= 视频宽度
            //   例如 1920 宽的视频，stride 可能是 1920 或 2048（对齐到 128）
            //
            // 审查点（fd 所有权）：objects[0].fd 归 frame 所有，本函数绝不
            // close/munmap——RGA 同步调用在 av_frame_unref(frame) 之前完成，
            // 因此 unref 释放 surface 时 RGA 已读完，生命周期安全。
            //
            // 审查点（vstride 假设）：这里把 RGA 的 vstride 参数直接传
            // frame->height（见 4.5 调用），隐含假设 MPP 输出无垂直对齐填充。
            // 若某些分辨率下 UV 平面按 16 行对齐产生 padding，色度平面会错位
            // （画面偏色/横条纹）。更稳健的做法见 camera_preview_decoder.cpp：
            // 用 planes[1].offset - planes[0].offset 再除以 pitch 反推真实 vstride。
            int src_fd = -1;
            int src_stride = vid_w;

            if (frame->format == AV_PIX_FMT_DRM_PRIME)
            {
                AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
                if (desc && desc->nb_objects > 0)
                {
                    src_fd = desc->objects[0].fd;           // DMA-BUF fd
                    src_stride = desc->layers[0].planes[0].pitch;  // 实际 stride
                }
            }

            if (src_fd >= 0)
            {
                // ===== 4.5 RGA 色彩转换：NV12 → RGBA（零拷贝） =====
                //
                // 这是整个流程中最关键的一步：
                //   输入：MPP 解码输出的 NV12 DMA-BUF fd（GPU 显存中）
                //   输出：RGBA DMA-BUF fd（同样在 GPU 显存中）
                //   过程：RGA 硬件直接读写 GPU 显存，CPU 完全不参与
                //
                // 为什么需要色彩转换？：
                //   - MPP 解码输出：NV12（YUV 4:2:0）—— 视频压缩的标准格式
                //   - 显示/推理需要：RGBA/RGB —— 显示器和 AI 模型的标准格式
                //   - 转换公式：YUV → RGB 需要矩阵运算，CPU 做很慢
                //
                // 调用链：
                //   RgaConverter::convertNV12ToRGBAbyRGA()
                //     → importbuffer_fd(): 将 DMA-BUF fd 导入 RGA
                //     → wrapbuffer_handle(): 包装成 RGA 可操作的 buffer
                //     → imcvtcolor(): RGA 硬件执行色彩转换
                //     → releasebuffer_handle(): 释放 RGA 资源

                DmaFrameBuffer* dst_buf = dst_bufs[back_buf];
                bool ok = RgaConverter::convertNV12ToRGBAbyRGA(
                    src_fd, 
                    dst_buf->fd(), 
                    frame->width, 
                    frame->height, 
                    src_stride, 
                    frame->height);

                if (ok)
                {
                    // ===== 4.6 每 3 帧做一次推理（降频处理） =====
                    //
                    // 为什么不是每帧都推理？：
                    //   - 视频通常是 25-30 fps，但推理速度可能只有 10-15 fps
                    //   - 每 3 帧推理一次 = 约 8-10 fps 的检测频率
                    //   - 足够用于实时告警，同时留出算力给其他通道
                    //
                    if (frame_count % 3==0) {
                        
                        // 从 DMA 缓冲池获取一个可用的 RGB 缓冲区（非阻塞）
                        // 缓冲池预分配了 16 个 640×640 的 RGB 缓冲区
                        // tryAcquireSharedPtr(): 返回 shared_ptr，引用计数归零时自动归还
                        auto rgabuffer = dmaBufferPool_->tryAcquireSharedPtr();
                        if (rgabuffer)
                        {
                            // ===== 4.7 RGA 缩放 + Letterbox：RGBA → RGB 640×640 =====
                            //
                            // 这是送入模型前的最后一步预处理：
                            //   输入：原始分辨率的 RGBA（如 1920×1080）
                            //   输出：640×640 的 RGB（YOLO 模型的标准输入尺寸）
                            //
                            // Letterbox 处理：
                            //   - 保持原始宽高比，等比缩放到 640×640
                            //   - 不匹配的区域用灰色(114,114,114)填充
                            //   - 这是 YOLO 系列模型的标准预处理方式
                            //
                            // 调用链：
                            //   RgaConverter::rgba_to_rgb_resize()
                            //     → importbuffer_fd(): 导入 RGBA 缓冲区
                            //     → imfill(): 填充灰色背景
                            //     → improcess(): RGA 硬件执行缩放+转换
                            //     → RGB 输出缓冲区可直接送入 RKNN NPU
                            RgaConverter::rgba_to_rgb_resize(dst_buf->fd(), dst_buf->width(), dst_buf->height(), dst_buf->stride(), 
                                                                rgabuffer->fd,  rgabuffer->width, 
                                                                rgabuffer->height, rgabuffer->width_stride, true);
                            
                            // 封装图像信息，准备送入推理任务
                            std::shared_ptr<image_buffer_t>  image = std::make_shared<image_buffer_t>();
                            image->format = image_format_t::IMAGE_FORMAT_RGB888;
                            image->virt_addr = (uint8_t*)(rgabuffer->va);  // RGB 像素的虚拟地址
                            image->width = 640;
                            image->height = 640;
                            image->width_stride = 640;
                            image->srcWidth = dst_buf->width();   // 原始宽度（用于后处理映射坐标）
                            image->srcHeight = dst_buf->height(); // 原始高度
                            image->sp_dmaBuffer = rgabuffer;      // 保持引用，防止缓冲区被回收
                            auto t = chrono::system_clock::now();
                            // 时间戳取 time_since_epoch().count()：Linux/libstdc++ 下
                            // system_clock 的 tick 周期是 1ns，所以这里是"epoch 纳秒"。
                            // 该值随 TaskData 一路传到 PpeTask::run() 回填
                            // od_results.time（设计意图：结果携带的是"采集时刻"而非
                            // "推理完成时刻"，便于统计端到端延迟并让 PriorityQueue
                            // 始终保留最新帧）。
                            // ⚠ 单位隐患：common.hpp 把 results.time 注释为"毫秒"，
                            //   alarm_manager.h 的限流常量又按纳秒值(2e9)与其相减比较，
                            //   三处口径不一致，详见 common.hpp / alarm_manager.h 警示。
                            image->time = t.time_since_epoch().count();  // 时间戳（用于排序）

                            // ===== 4.8 将帧分发给所有级联推理任务 =====
                            //
                            // 级联模型机制：
                            //   - 模型1（主模型）：检测所有类别
                            //   - 模型2-5（可选）：针对特定场景优化
                            //   - 每个模型独立推理，结果写入各自的队列
                            //   - 解码线程合并所有结果，用不同颜色画框
                            //
                            // TaskData 包含：
                            //   - 时间戳（用于优先级排序）
                            //   - 图像数据（shared_ptr，多个任务共享）
                            //   - 结果队列（推理结果写回这里）
                            auto now = chrono::system_clock::now();
                            // TaskData 的排序时间戳（epoch 纳秒）：PpeTask::run()
                            // 用 taskData->time 回填 od_results.time，即结果队列里
                            // 的 time 来自这里（与上面 image->time 同源同单位，
                            // 但取样时刻略晚于 RGA 缩放完成时）
                            long ts = now.time_since_epoch().count();
                            for (size_t k = 0; k < tasks_.size(); ++k) {
                                auto td = std::make_shared<TaskData>(ts, image, slotQueues_[k]);
                                tasks_[k]->put(td);  // 非阻塞放入，如果队列满则丢弃旧帧
                            }
                        }
                    }
                    
                    // ===== 4.9 合并推理结果并画框 =====
                    //
                    // 级联模型结果合并：
                    //   每个模型的推理结果在各自的 slotQueues_[k] 里
                    //   这里依次取出，用不同颜色画到同一帧上
                    //   文字前缀 M{槽位}，如 "M1 person 87.3%"
                    //
                    // 颜色方案：
                    //   模型1=蓝, 模型2=绿, 模型3=红, 模型4=黄, 模型5=橙
                    //   一眼能分清是哪个模型检出的
                    const int kSlotColors[5] = {
                        (int)COLOR_BLUE, (int)COLOR_GREEN, (int)COLOR_RED,
                        (int)COLOR_YELLOW, (int)COLOR_ORANGE
                    };

                    // 构造显示用的图像结构（指向 RGBA 缓冲区）
                    image_buffer_t dislayImage;
                    dislayImage.format = image_format_t::IMAGE_FORMAT_RGBA8888;
                    dislayImage.virt_addr = (unsigned char*)dst_buf->ptr();
                    dislayImage.width = dst_buf->width();
                    dislayImage.height = dst_buf->height();

                    for (size_t k = 0; k < slotQueues_.size(); ++k) {
                        // 从该模型的结果队列中取最新一次检测结果
                        // tryPop: 非阻塞，如果队列为空则跳过
                        object_detect_result_list od;
                        if (!slotQueues_[k]->tryPop(od)) continue;

                        int color = kSlotColors[(int)k % 5];
                        char text[256];
                        for (int i = 0; i < od.count; i++) {
                            const object_detect_result &d = od.results[i];
                            // 画检测框
                            draw_rectangle(&dislayImage,
                                           d.box.left, d.box.top,
                                           d.box.right - d.box.left,
                                           d.box.bottom - d.box.top,
                                           color, 3);

                            // 画标签文字：M1 person 87.3%（用该槽位自己的类名表）
                            const std::vector<std::string> &slotNames = slotClassNames_[k];
                            if (d.cls_id >= 0 && d.cls_id < (int)slotNames.size())
                                snprintf(text, sizeof(text), "M%zu %s %.1f%%",
                                         k + 1, slotNames[d.cls_id].c_str(), d.prop * 100);
                            else
                                snprintf(text, sizeof(text), "M%zu cls_%d %.1f%%",
                                         k + 1, d.cls_id, d.prop * 100);
                            draw_text(&dislayImage, text, d.box.left, d.box.top - 20, color, 10);
                        }

                        // ===== 4.10 电子围栏判断 + 告警 =====
                        // 流程：
                        //   1. 检查围栏是否启用且该通道有围栏
                        //   2. 如果有围栏：过滤出在围栏内的目标，再判断是否触发报警
                        //   3. 如果没有围栏：直接使用全部检测结果判断报警
                        //   4. 围栏模式下，只统计在围栏内的目标（inside_alarm模式）
                        auto &fenceMgr = geofence::FenceManager::instance();
                        bool fenceEnabled = fenceMgr.enabled();
                        bool hasFence = fenceEnabled && fenceMgr.hasFence(channel_);
                        bool alarmInside = (fenceMgr.mode() == "inside_alarm");

                        QVector<AlarmRecord> newAlarms;
                        if (hasFence) {
                            // 围栏模式：需要先过滤出在围栏内的目标
                            const geofence::ChannelFence &cf = fenceMgr.channelFence(channel_);
                            const QStringList &fenceClasses = fenceMgr.alarmClasses();
                            int oW = 0, oH = 0;
                            fenceMgr.overlaySize(channel_, oW, oH);

                            // 汇总信息：输出围栏检测的配置参数
                            const QStringList &alarmCls = AlarmManager::instance().alarmClasses();
                            fenceMgr.postLog("fence",
                                QString("[通道%1] 围栏检测: %2个目标, 模式=%3, 围栏类别=%4, 报警类别=%5, 围栏=%6个")
                                    .arg(channel_ + 1).arg(od.count).arg(fenceMgr.mode())
                                    .arg(fenceClasses.isEmpty() ? "person" : fenceClasses.join(","))
                                    .arg(alarmCls.isEmpty() ? "(空)" : alarmCls.join(","))
                                    .arg(cf.shapes.size()));

                            object_detect_result_list filteredOd;
                            filteredOd.id = od.id;
                            filteredOd.time = od.time;
                            filteredOd.count = 0;
                            for (int i = 0; i < od.count; i++) {
                                const auto &d = od.results[i];
                                // 按该结果所属槽位取类名（与报警侧同口径）
                                QString clsName = AlarmManager::instance()
                                                      .resolveClassName(od.id, d.cls_id);

                                // 类别检查：只处理围栏类别中定义的目标
                                bool classMatch = fenceClasses.isEmpty() ||
                                                  fenceClasses.contains(clsName);
                                if (!classMatch) {
                                    fenceMgr.postLog("fence",
                                        QString("  [通道%1] 目标%2: %3 不在围栏类别中，跳过")
                                            .arg(channel_ + 1).arg(i).arg(clsName));
                                    continue;
                                }

                                // 围栏判定：检查目标中心点是否在围栏区域内
                                // alarmInside=true：目标在围栏内报警；false：目标在围栏外报警
                                bool inside = geofence::FenceChecker::checkDetection(
                                        d,
                                        dislayImage.width, dislayImage.height,
                                        640, 640, oW, oH,
                                        cf, alarmInside);

                                int footX = (d.box.left + d.box.right) / 2;
                                int footY = d.box.bottom;
                                fenceMgr.postLog("fence",
                                    QString("  [通道%1] 目标%2: %3  foot=(%4,%5) %6")
                                        .arg(channel_ + 1).arg(i).arg(clsName)
                                        .arg(footX).arg(footY)
                                        .arg(inside ? "→ 在围栏内 ✓" : "→ 在围栏外"));

                                // 只保留在围栏内的目标
                                if (inside) {
                                    if (filteredOd.count < OBJ_NUMB_MAX_SIZE) {
                                        filteredOd.results[filteredOd.count++] = d;
                                    }
                                }
                            }

                            // 过滤汇总：输出围栏内目标数量
                            fenceMgr.postLog("fence",
                                QString("  [通道%1] 过滤结果: %2/%3 个目标在围栏内")
                                    .arg(channel_ + 1).arg(filteredOd.count).arg(od.count));

                            // 使用正常限流（2秒内同通道同类别只报1次），避免报警洪水
                            // bypassThrottle=false：围栏报警也受2秒限流
                            newAlarms = AlarmManager::instance().ingest(channel_, filteredOd, false);
                            for (auto &a : newAlarms) a.isFenceAlarm = true;

                            // 报警结果：输出围栏报警触发信息
                            if (!newAlarms.isEmpty()) {
                                fenceMgr.postLog("alarm",
                                    QString("  [通道%1] ★ 围栏报警触发: %2 (置信度 %3%)")
                                        .arg(channel_ + 1)
                                        .arg(newAlarms.front().className)
                                        .arg(newAlarms.front().confidence * 100, 0, 'f', 1));
                            } else if (filteredOd.count > 0) {
                                // ingest 返回空，说明类别不在 alarmClasses 中
                                fenceMgr.postLog("warning",
                                    QString("  [通道%1] ⚠ 有%2个目标在围栏内但未生成报警 - "
                                            "请检查'报警类别'是否包含围栏检测的目标类别")
                                        .arg(channel_ + 1).arg(filteredOd.count));
                            }
                        } else {
                            // 无围栏模式：直接使用全部检测结果判断报警
                            // bypassThrottle=false：普通检测报警也受2秒限流
                            newAlarms = AlarmManager::instance().ingest(channel_, od);
                        }
                        //
                        // AlarmManager::ingest() 处理流程：
                        //   1. 统计各类别检测次数
                        //   2. 检查类别是否在报警类别列表中
                        //   3. 去重限流：同通道同类别 2 秒只报 1 次（bypassThrottle=true 时跳过限流）
                        //   4. 创建 AlarmRecord 并返回
                        //
                        // 截图机制：
                        //   - 解码线程只负责拷贝像素 + 提交（不阻塞）
                        //   - PNG/JPEG 编码在独立后台线程完成
                        //   - 避免编码耗时拖低帧率
                        if (!newAlarms.isEmpty()) {
                            QString snap;
                            if (AlarmManager::instance().screenshotsEnabled()) {
                                // 提交截图任务（异步，不等待）
                                snap = submitAlarmSnapshot(
                                    channel_, newAlarms.front().className,
                                    dislayImage.width, dislayImage.height,
                                    (const unsigned char *)dislayImage.virt_addr);
                                // 只在截图成功时设置路径
                                for (auto &a : newAlarms) a.imagePath = snap;
                            }
                            // 入库并通知界面（弹 toast / 报警列表新增）
                            // storeAndNotify 流程：
                            //   1. 将报警添加到内存列表（最多保留1000条）
                            //   2. 写入 SQLite 数据库
                            //   3. 发送 alarmGenerated 信号通知界面刷新
                            AlarmManager::instance().storeAndNotify(newAlarms);
                        }
                    }

                    
                    // ===== 4.11 将 RGBA 帧发送到 UI 渲染 =====
                    //
                    // RenderFrame 包含 DMA-BUF fd，渲染线程可以直接使用
                    // dup(fd): 复制 fd，因为当前帧的缓冲区会被下一轮覆盖
                    //   - 不 dup 的话，渲染线程还没用完，解码线程就开始覆盖了
                    //   - dup 的 fd 有独立的引用计数，两边都可以独立 close
                    //
                    // emit frameReady(): 通过 Qt 信号传递到 UI 线程
                    //   - UI 线程用 OpenGL/EGL 将 DMA-BUF 渲染到屏幕上
                    //   - 整个过程零拷贝：解码→RGA→渲染 全在 GPU 显存中完成
                    RenderFrame rf;
                    rf.fd = dup(dst_buf->fd());
                    rf.width = vid_w;
                    rf.height = vid_h;
                    rf.stride = dst_buf->stride();

                    // 视频录制：将帧编码并存入环形缓冲区
                    // 在报警时可以 dump 最近 N 秒的视频
                    if (videoRecordingEnabled_ && videoRecorder_ && videoRecorder_->isInitialized()) {
                        // 获取当前时间戳（毫秒）
                        qint64 timestamp_ms = QDateTime::currentMSecsSinceEpoch();
                        // RGBA 数据在 dst_buf->ptr() 中，直接读取编码
                        videoRecorder_->encodeFrame(
                            (const unsigned char*)dst_buf->ptr(),
                            vid_w, vid_h, timestamp_ms);
                    }

                    emit frameReady(rf);
                   
                }

                // 切换双缓冲：这次写 0 号缓冲，下次写 1 号缓冲
                // 避免 RGA 还在读 0 号时，解码器就开始写 0 号
                back_buf = 1 - back_buf;
            }

            // 释放帧的引用，允许 FFmpeg 重用底层内存
            av_frame_unref(frame);

            // ===== 4.12 帧率控制（30fps） =====
            //
            // 如果解码+处理太快，主动等待以维持 30fps
            // 预期每帧耗时 33333 微秒（1000000/30，取整略小于 1/30s）
            // 实际耗时 < 预期时，sleep 补齐差值
            // 审查点：expected 随 frame_count 线性累积，若中途某帧处理超时，
            // 后续帧不会"追帧"（sleep 条件自动失效），等价于平滑限速而非
            // 严格节拍；QElapsedTimer::elapsed() 单位是 ms，故 *1000 换算成 us。
            //
            // 为什么需要帧率控制？：
            //   - RTSP 流通常是 25-30fps
            //   - 如果解码太快，缓冲区会堆积大量待处理帧
            //   - 主动等待可以减少延迟和内存占用
            qint64 expected_us = frame_count * 33333;
            qint64 elapsed_us = timer.elapsed() * 1000;
            if (expected_us > elapsed_us)
            {
                QThread::usleep(expected_us - elapsed_us);
            }

            t.tok();
        }
    }

    qDebug() << "Decode finished, frames:" << frame_count;

    emit statusChanged(channel_, 0);

    // ===== 步骤5: 清理资源 =====
    //
    // 释放顺序很重要：
    //   1. 释放 DMA-BUF 缓冲区（GPU 显存）
    //   2. 关闭 DRM 设备
    //   3. 释放 FFmpeg 帧/包/解码器上下文
    //   4. 关闭输入流
    //
    // 注意：avcodec_free_context() 会自动 flush 解码器，无需额外处理
    //
    // 审查点（释放配对核查）：
    //   - frame/pkt：与 av_frame_alloc/av_packet_alloc 一一对应，
    //     循环内所有 continue/break 路径都保持 unref 语义，无泄漏。
    //   - dec_ctx 引用了 pHWDeviceCtx 的 buffer(ref)，free_context 时释放；
    //     但本地这份 av_buffer_ref 前的"创建引用"未再 av_buffer_unref，
    //     每次开流泄漏 1 个 hwdevice buffer 引用（量小、单流一次性，
    //     如要求严格可对等补 unref——注释登记，不改代码）。
    //   - dst_bufs[2] 与 new 配对 delete；rgba_bufs 从未 alloc 无需释放。
    for (int i = 0; i < 2; i++)
    {
        delete dst_bufs[i];
        dst_bufs[i] = nullptr;
    }
    close(drm_fd);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    emit finished();
}
