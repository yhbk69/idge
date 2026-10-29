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
//   - 帧捕获槽（capturePtr_/captureW_/…）是给智能识别页的"拉最新一帧"接口，
//     不是帧交付队列：只有双缓冲，指针约两帧后被覆盖，取到必须立刻读完；
//     stop() 会先把它置空再释放 dst_bufs。因此不要在解码线程之外长期持有该指针。
//
// ============================================================================

#include "ffmpeg_video_decoder.h"
#include "runtime_paths.h"
#include "rga_converter.h"
#include "perf_probe.h"
#include "qt_image_utils.h"   // thumbnailSidecarPath：抓拍 sidecar 的命名规则只留一份
#include "model_registry.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <cerrno>
#include <queue>
#include <deque>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <cstring>
#include <cmath>
#include <algorithm>   // std::remove_if：围栏事件表淘汰
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include "DmaFrameBuffer.h"
#include "dma_alloc.h"        // dma_sync_device_to_cpu / dma_sync_cpu_to_device
#include <libswscale/swscale.h>   // 软件帧（摄像头裸流/纯软解）→ RGBA 兜底转换
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
// 目录：data/alarms/日期/通道N_类别_时间.jpg
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
        writeThumb(job);
    }

    // 顺带写一张 sidecar 缩略图，让报警面板不必在 GUI 线程解 720p。
    //
    // 【为什么写在这里】面板一次刷 20 张 64×48，读侧即便用 IMREAD_REDUCED_COLOR_8
    //   也要 11~22ms/张（Huffman 得把全部 MCU 解完，降采样只省 IDCT），
    //   冷启动 20 张实测 261ms，是当时唯一还能把 4 路 paintGL 一起顶到 374ms 的东西。
    //   本函数在 SnapWriter 后台线程，解码线程只 submit 不等待，所以这里多花几毫秒
    //   换掉 GUI 线程的十几毫秒是净赚。
    // 【尺寸口径】与读侧 loadThumbnailScaled 一致：等比装进 64×48 框，
    //   16:9 源出 64×36，读侧 ratio==1 直接用、不再二次缩放。
    // 【色彩】job.rgb 是 RGB 序，cv::imwrite 按 BGR 解释 → 必须先转一次。
    static void writeThumb(SnapJob &job)
    {
        static const int kBoxW = 64, kBoxH = 48;
        if (job.w <= 0 || job.h <= 0) return;
        const double ratio = std::min(double(kBoxW) / job.w, double(kBoxH) / job.h);
        if (ratio >= 1.0) return;   // 源图本来就比较小，sidecar 没有收益

        cv::Mat rgb(job.h, job.w, CV_8UC3, job.rgb.data());
        cv::Mat bgr, thumb;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        cv::resize(bgr, thumb, cv::Size(int(job.w * ratio), int(job.h * ratio)),
                   0, 0, cv::INTER_AREA);
        const QString thumbPath = thumbnailSidecarPath(job.path);
        if (!cv::imwrite(thumbPath.toUtf8().constData(), thumb))
            qWarning() << "alarm thumbnail write failed" << thumbPath;
    }

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<SnapJob> q_;
    bool stop_ = false;
};

// ============================================================
// 报警视频片段异步写入器
//
// 与 SnapWriter 同一套范式（进程内唯一后台线程 + 有界队列），多一个"延迟"维度：
//   片段要覆盖"报警前 3 秒 + 报警后 2 秒"，而后 2 秒在提交那一刻还没发生，
//   所以任务带着 due 时刻（报警时刻 + post），到点才从环形缓冲区取窗口。
//   解码线程只提交、不等待，绝不在这里同步 dump（否则帧率骤降）。
//
// 为什么丢最旧而不是丢最新：环形缓冲区只保留最近 bufferSeconds 秒，
// 队列积压时队首任务的窗口早就被新包覆盖，dump 出来是残缺片段；
// 丢掉它反而给后面的新任务腾出位置。
// ============================================================

// 片段窗口：D4 决策"前 3 秒 + 后 2 秒"，与 recording.buffer_seconds 之和等量
static constexpr qint64 kClipPreMs = 3000;
static constexpr qint64 kClipPostMs = 2000;

// 围栏"进入沿触发"阈值（见 FFmpegVideoDecoder::FenceInsideTrack）：
//   IoU 0.3 比 BoxSmoother 绘图用的 0.2 严——这里误匹配会把两次独立入侵并成一个事件，
//   代价是漏报警；绘图误匹配只是框抖一下，容忍度不同。
//   保活 1.5 秒：推理约 10Hz（每 3 帧一次），够跨过偶发漏检与短暂遮挡，
//   又短到"走出去再回来"能被认成新事件。
static constexpr float kFenceTrackIouThr = 0.3f;
static constexpr long long kFenceTrackKeepNs = 1500LL * 1000000LL;

struct ClipJob {
    std::shared_ptr<VideoRecorder> recorder;   // 持有引用计数：解码器已停止也不会悬垂
    QStringList alarmIds;                      // 本片段对应的一批报警（同通道同瞬间只出一个片段）
    QString rawPath;                           // Annex-B 裸流（上传服务器用）
    QString mp4Path;                           // 同名 MP4 副本（网页/播放器用）
    qint64 centerMs = 0;                       // 窗口中心 = 报警时刻（本机毫秒墙钟）
    qint64 dueMs = 0;                          // 到点才 dump：centerMs + kClipPostMs
};

class ClipWriter {
public:
    static ClipWriter &get()
    {
        static ClipWriter w;                // 首次提交时启动
        return w;
    }

    // 提交一个延迟 dump 任务，立即返回（解码线程不等待）
    void submit(ClipJob job)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (static_cast<int>(q_.size()) >= kMaxPending) q_.pop_front();   // 满了丢最旧
        q_.push_back(std::move(job));
        cv_.notify_one();
    }

private:
    static const int kMaxPending = 8;   // 报警限流 2s/次 × 4 路，8 段足够缓冲突发

    ClipWriter() { th_ = std::thread([this] { run(); }); }
    ~ClipWriter()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();   // 退出前把队列里剩余的片段都落盘
    }

    void run()
    {
        for (;;) {
            ClipJob job;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                for (;;) {
                    if (stop_ && q_.empty()) return;
                    if (!q_.empty()) {
                        const qint64 now = QDateTime::currentMSecsSinceEpoch();
                        // 队首到了 dump 时刻（或正在退出）就取走，否则睡到点
                        if (stop_ || now >= q_.front().dueMs) break;
                        cv_.wait_for(lk, std::chrono::milliseconds(q_.front().dueMs - now),
                                     [this] { return stop_; });
                        continue;
                    }
                    cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                }
                job = std::move(q_.front());
                q_.pop_front();
            }
            writeClip(job);
        }
    }

    // 真正取窗口 + 落盘 + 回填报警（只在后台线程调用）
    static void writeClip(ClipJob &job)
    {
        if (!job.recorder || !job.recorder->isInitialized()) {
            return;
        }
        if (!job.recorder->dumpAnnexB(job.rawPath, job.centerMs, kClipPreMs, kClipPostMs)) {
            return;   // dumpAnnexB 内部已 qWarning，且不留残档
        }
        // MP4 只是网页可播的副本，失败不影响报警本身（裸流已落盘）
        const bool mp4Ok = job.recorder->dumpMp4(job.mp4Path, job.centerMs, kClipPreMs, kClipPostMs);

        // 两次 collectWindow 取到的是同一批包：本任务到点时墙钟已达
        // center+post，之后入环的包 wall_ms 必然 > 窗口上界，不会再进窗口。
        AlarmManager::instance().attachVideoClip(job.alarmIds, job.rawPath);
        qInfo() << "Alarm clip:" << job.rawPath << "mp4" << (mp4Ok ? "ok" : "skipped");
    }

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<ClipJob> q_;
    bool stop_ = false;
};

} // namespace

// 生成本通道片段路径并提交延迟 dump。
// 这里不返回路径：文件此刻还不存在（要等 post 秒），videoPath 由 ClipWriter
// 在真正落盘后通过 AlarmManager::attachVideoClip() 回填，避免报警记录指向空文件。
//
// 目录/命名与截图完全同规则（data/alarms/yyyyMMdd/chN_类别_时分秒毫秒.*），
// 只是扩展名换成裸流与 MP4，便于同一报警的图片/视频成对查找。
// VideoRecorder 本身不知道通道与路径（保持与业务无关），所以在这里拼。
static void submitAlarmClip(const std::shared_ptr<VideoRecorder> &recorder,
                            int channel, const QString &className,
                            const QStringList &alarmIds, qint64 centerMs)
{
    if (!recorder || !recorder->isInitialized() || alarmIds.isEmpty()) {
        return;
    }

    const QString day = QDate::currentDate().toString("yyyyMMdd");
    const QString dir = RuntimePaths::alarmsDir() + QStringLiteral("/") + day;
    if (::mkdir(dir.toUtf8().constData(), 0755) != 0 && errno != EEXIST) {
        return;
    }

    // 扩展名跟实际启用的编码器走：hevc_rkmpp → .h265，h264_rkmpp → .h264
    // （退档时编码器已换，只认配置会与实际产物不符）
    const QString ext = recorder->encoderName().startsWith(QStringLiteral("hevc"))
                            ? QStringLiteral("h265") : QStringLiteral("h264");
    const QString stem = QString("%1/ch%2_%3_%4")
        .arg(dir)
        .arg(channel + 1)
        .arg(className)
        .arg(QDateTime::currentDateTime().toString("HHmmss_zzz"));

    ClipJob job;
    job.recorder = recorder;
    job.alarmIds = alarmIds;
    job.rawPath = stem + QStringLiteral(".") + ext;
    job.mp4Path = stem + QStringLiteral(".mp4");
    job.centerMs = centerMs;
    job.dueMs = centerMs + kClipPostMs;
    ClipWriter::get().submit(std::move(job));
}

// 生成截图路径并提交给后台线程，返回保存路径
static QString submitAlarmSnapshot(int channel, const QString &className,
                                   int w, int h, const unsigned char *rgba)
{
    // 一级目录 data/alarms/ 存截图根目录
    const QString alarmRoot = RuntimePaths::alarmsDir();
    if (::mkdir(alarmRoot.toUtf8().constData(), 0755) != 0 && errno != EEXIST) {
        return QString();
    }
    // 二级目录 data/alarms/yyyyMMdd 按天归档
    QString day = QDate::currentDate().toString("yyyyMMdd");
    QString dir = alarmRoot + QStringLiteral("/") + day;
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
// 报警出口：截图 → 入库并通知界面 → 提交视频片段
// ============================================================================
// 普通检测报警与围栏报警共用同一段收尾逻辑（此前只在槽位循环里内联写了一份，
// 围栏判定挪到通道级之后就有两个调用点了）。
//
// 一批报警只出一张截图、一个片段：同通道同瞬间的多类别报警共享同一帧带框画面，
// 命名取 front().className。
//
// 顺序要求：先 storeAndNotify 再 submitAlarmClip——attachVideoClip 的 UPDATE
// 必须能命中已存在的行（片段队列一空就可能立刻执行）。
// ============================================================================
static void deliverAlarms(int channel, QVector<AlarmRecord> &alarms,
                          const image_buffer_t &frame,
                          const std::shared_ptr<VideoRecorder> &recorder)
{
    if (alarms.isEmpty())
        return;

    if (AlarmManager::instance().screenshotsEnabled()) {
        const QString snap = submitAlarmSnapshot(
            channel, alarms.front().className, frame.width, frame.height,
            (const unsigned char *)frame.virt_addr);
        // 只在截图成功时设置路径
        for (auto &a : alarms) a.imagePath = snap;
    }

    AlarmManager::instance().storeAndNotify(alarms);

    if (recorder) {
        QStringList alarmIds;
        alarmIds.reserve(alarms.size());
        for (const auto &a : alarms) alarmIds << a.id;
        submitAlarmClip(recorder, channel, alarms.front().className, alarmIds,
                        QDateTime::currentMSecsSinceEpoch());
    }
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
//
// PreviewOnly 模式两件事都不做：既不加载 RKNN 模型，也不预留 64 个
// 640×640 DMA 缓冲（智能识别 QR/OCR 只要一帧原始画面，推理链路整条不建）。
//
// 推理任务不在这里建：四路 PlayerWidget 是 .ui 提升控件，构造时拿不到通道号，
// 而按通道配置的槽位必须知道 channel_ 才能解析，所以挪到 start()
// （PlayerWidget::open() 是 setChannel(ch) 紧接 start(url)，此时 channel_ 已就位）。
// ============================================================================
FFmpegVideoDecoder::FFmpegVideoDecoder(QObject *parent, InferenceMode mode)
    : QObject(parent)
      , inferenceEnabled_(mode == InferenceMode::Cascade)
{
    if (!inferenceEnabled_)
        return;

    // 创建 DMA 缓冲池：64 个 640×640 的 RGB 缓冲区
    dmaBufferPool_ = std::make_shared<DmaBufferPool>(64, 640, 640, RK_FORMAT_BGR_888, 16);
}

// ============================================================
// 级联任务构建（按通道）：
//   由 ModelRegistry::resolveChannelSlots(channel_) 展开本通道配置的模型库 id
//   列表（路径/标签/报警类别都从模型库+参数表取，config 里没有路径字符串）。
//   每个可用槽位建一个 PpeTask（独立推理线程）+ 一个结果队列；
//   taskConfig.result_id = 紧凑槽位下标，即通道 models[] 里生效项的顺序。
//   NPU 核用进程级轮转计数器分配：四路解码器各自 slotIdx%3 会让每个通道都
//   从 core0 开始，3 核被前两个通道占满后其余通道只能挤同一核。
//   模型列表为空 = 这一路不推理（画面照常显示），只会打一条告警。
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
    if (!tasks_.empty())
        return;  // 任务集合只在 tasks_ 为空时建立。改模型列表请先 releaseTasks()
                 // 再 start()（设置页「模型…」即走这条路）；只改报警类别用 updateSlotAlarmClasses()。

    ConfigManager &cfg = ConfigManager::instance();
    // 正常启动时 frmmain 早已 load()；这里只兜"直接 new 解码器"这种用法下配置还没读进来。
    // 判据用 V2 的必有键：模型参数表与通道模型列表同时为空即视为未加载。
    if (cfg.modelParamKeys().isEmpty() && cfg.channelModelIds(channel_).isEmpty())
        cfg.load(RuntimePaths::configFile());

    // 本通道的槽位：模型路径 + 标签 + 报警类别（缺失项已被跳过并告警）
    QList<ResolvedSlot> resolved = ModelRegistry::resolveChannelSlots(channel_);
    if (resolved.isEmpty()) {
        // 兜底：这一路没配模型（或配的全不在库中），回退库内默认模型，
        // 保证画面仍有检测框，而不是整路不建推理任务。
        const QString fbId = QStringLiteral("yolo11n-coco");
        const QString fbModel = ModelRegistry::instance().modelPath(fbId);
        if (fbModel.isEmpty() || !QFile::exists(fbModel)) {
            qWarning() << "[decoder] 通道" << channel_
                       << "无可用模型，库内默认模型也缺失，本路不建推理任务";
            return;
        }
        ResolvedSlot fb;
        fb.libId = fbId;
        fb.modelPath = fbModel;
        fb.labelPath = ModelRegistry::instance().labelPath(fbId);
        fb.alarmClass = cfg.modelAlarmClass(fbId);
        qWarning() << "[decoder] 通道" << channel_ << "未配置可用模型，回退库内默认:" << fbId;
        resolved.append(fb);
    }

    // 进程级轮转：核掩码是位标志（CORE_0=1/CORE_1=2/CORE_2=4），
    // 四路解码器共享一个计数器才能让三个核真的被轮流用上。
    static std::atomic<unsigned> nextCore{0};
    static std::atomic<int> totalTasks{0};
    static constexpr rknn_core_mask kCoreMasks[] = {
        RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2
    };

    // 每个可用槽位一个推理任务 + 一个结果队列 + 一份类名表
    for (const ResolvedSlot &s : resolved) {
        const QString mp = s.modelPath;
        const QString lp = s.labelPath;
        const int slotIdx = (int)tasks_.size();  // 紧凑下标，即 result_id

        TaskConfig taskConfig;
        taskConfig.core_mask = kCoreMasks[nextCore.fetch_add(1) % 3];
        taskConfig.modelPath = mp.toStdString();
        taskConfig.labelPath = lp.toStdString();
        taskConfig.result_id = slotIdx;

        // 类别数：优先取模型库元数据，库外模型按标签行数估算
        const ModelMeta meta = ModelRegistry::instance().meta(s.libId);
        taskConfig.numClasses = meta.numClasses > 0
                                    ? meta.numClasses
                                    : (int)readLabelNames(lp).size();

        PpeTask *task = new PpeTask(taskConfig);
        tasks_.push_back(task);
        // 每路 12 个上下文、启动一次，打印通道/槽位/绑定核，便于现场核对
        // NPU 三核是否真的都在用（debugfs 的 per-core load 需要 root，板端一般读不到）。
        const int created = totalTasks.fetch_add(1) + 1;
        qInfo() << "cascade slot ch" << channel_ << "slot" << slotIdx
                << "npu_core_mask" << (int)taskConfig.core_mask << mp;
        if (created > 6)
            qWarning() << "[decoder] 全进程推理任务已达" << created
                          << "个（3 个 NPU 核），建议减少部分通道的模型数量";

        // 每个任务一个独立结果队列（容量 8，满了丢旧保新）
        slotQueues_.push_back(std::make_shared<PriorityQueue<object_detect_result_list>>(8));

        // 类名表：画框用 std 版，报警侧用 QString 版（同一数据源，同下标）
        slotClassNames_.push_back(readLabelNames(lp));

        // 报警类别是"模型的参数"（video.models[<id>].alarm_class），空 = 只画框不报警。
        // 存在本解码器里而非 AlarmManager 单例：按通道配置后，通道 0 的槽位 1 与
        // 通道 2 的槽位 1 是两个不同模型，写进同一张全局表会互相覆盖。
        // 加锁是因为运行期 GUI 可以经 updateSlotAlarmClasses() 改写这张表（槽位数不变时）。
        {
            std::lock_guard<std::mutex> lk(alarmClsMtx_);
            slotAlarmClasses_.push_back(s.alarmClass);
        }
    }

    qDebug() << "Cascade tasks channel" << channel_ << ":" << tasks_.size();
    for (size_t k = 0; k < tasks_.size(); ++k) {
        qDebug() << "  slot" << k << "->" << QString::fromStdString(tasks_[k]->getModelPath())
                 << "labels:" << (int)slotClassNames_[k].size()
                 << "alarm_class:" << slotAlarmClasses_[k];
    }

    // 围栏前置条件：围栏是通道级判定，需要本通道至少有一个模型能产出围栏类别。
    // 挂了 PPE（只有 helmet/vest/un_helmet/…）却没挂 person 的通道，围栏永远不会触发——
    // 这种配置错误过去完全静默，这里显式说一声。
    {
        auto &fenceMgr = geofence::FenceManager::instance();
        if (fenceMgr.enabled() && fenceMgr.hasFence(channel_)) {
            const QString fenceCls = fenceMgr.alarmClass();
            bool producible = false;
            for (const auto &names : slotClassNames_) {
                for (const auto &n : names) {
                    if (QString::fromStdString(n) == fenceCls) { producible = true; break; }
                }
                if (producible) break;
            }
            if (!producible)
                qWarning() << "[geofence] 通道" << channel_ + 1 << "无模型产出类别"
                           << fenceCls << "，围栏不会触发；请给该通道加上含该类别的模型";
        }
    }
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { stop(); }

// ============================================================================
// releaseTasks: 丢弃本通道全部推理任务与槽位表（热更新用）
// ============================================================================
// 前提见头文件：解码循环已退出（isLoopAlive()==false）且由 GUI 线程调用。
// 之后下一次 start() 里 buildCascadeTasks() 会因 tasks_ 为空而按新配置重建，
// 槽位数/顺序/类名表/报警类别/围栏前置告警全部重新算一遍。
//
// 为什么这里可以 delete，而 stop() 不行：stop() 之后解码线程可能还在跑，
// 而本函数的调用点已确认旧循环退出、且每个任务的推理线程用 isThreadFinished()
// 逐个确认（finished_ 由 run() 退出时置位）。没确认的那些照旧不删，转入
// quarantine_ —— 宁可漏一个对象，也不在 detach 的线程脚下抽走内存。
void FFmpegVideoDecoder::releaseTasks()
{
    if (liveLoops_.load() > 0) {
        qWarning() << "[decoder] 通道" << channel_
                   << "解码循环仍在运行，releaseTasks 已拒绝（推理任务保持不变）";
        return;
    }

    int freed = 0;
    for (PpeTask *task : tasks_) {
        if (!task)
            continue;
        if (task->isThreadFinished()) {
            delete task;
            ++freed;
        } else {
            qWarning() << "[decoder] 通道" << channel_
                       << "有推理线程未确认退出，本槽对象暂不回收（隔离计数"
                       << (int)(quarantine_.size() + 1) << "）";
            quarantine_.push_back(task);
        }
    }

    tasks_.clear();
    slotQueues_.clear();
    slotClassNames_.clear();
    {
        std::lock_guard<std::mutex> lk(alarmClsMtx_);
        slotAlarmClasses_.clear();
    }
    qInfo() << "[decoder] 通道" << channel_ << "推理任务已释放" << freed << "个，等待按新配置重建";
}

// ============================================================================
// updateSlotAlarmClasses: 在线换报警类别（不换模型、不动槽位数）
// ============================================================================
// 只改写 slotAlarmClasses_：解码线程每帧在锁内拷一份去构造 SlotContext，
// 所以下一帧起报警判定就是新值。画框、类名表、推理任务一概不受影响。
bool FFmpegVideoDecoder::updateSlotAlarmClasses(const std::vector<QString> &cls)
{
    std::lock_guard<std::mutex> lk(alarmClsMtx_);
    if (cls.size() != slotAlarmClasses_.size()) {
        // 槽位数变了 = 模型列表改过，这条路做不了，交回调用方走 releaseTasks+重开
        return false;
    }
    slotAlarmClasses_ = cls;
    return true;
}

// ============================================================================
// 启动解码器
// ============================================================================
// 启动流程：
//   1. 停止之前的解码线程（如果正在运行）
//   2. 按本通道配置构建级联推理任务（每个模型一个线程），首次才建
//   3. 启动所有级联推理任务
//   4. 创建新的 Qt 线程运行 decodeLoop()
//
// 为什么建任务放在这里而不是构造函数：四路 PlayerWidget 是 frmvideowindow.ui 的
// 提升控件，构造时拿不到通道号；PlayerWidget::open() 是 setChannel(ch) 紧接
// start(url)，走到这里 channel_ 已经就位，才能按通道解析槽位。
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

    // 上一次 stop() 若超时遗弃了解码线程，其 decodeLoop 仍在本对象上运行；
    // 此时重启会造成两个 decodeLoop 并发（且 running_ 复活僵尸循环）。拒绝并提示重试。
    if (liveLoops_.load() > 0) {
        qWarning() << "Decoder channel" << channel_
                   << "still shutting down, start() refused";
        emit error("Decoder is still shutting down, retry later");
        return;
    }

    url_ = url;
    running_ = true;

    // 按本通道配置建立推理任务（PreviewOnly 模式整条推理链路不建；
    // 已有任务时是空操作，所以"换模型列表"必须先 releaseTasks() 再走到这里）
    if (inferenceEnabled_)
        buildCascadeTasks();

    // 启动所有级联推理任务（每个模型一个线程）
    for (PpeTask *task : tasks_) {
        task->start();
    }

    // 创建 Qt 线程，在线程中运行 decodeLoop()
    thread_ = new QThread;
    moveToThread(thread_);
    connect(thread_, &QThread::started, this, &FFmpegVideoDecoder::decodeLoop);
    connect(thread_, &QThread::finished, thread_, &QThread::deleteLater);

    // 线程结束时把本对象的归属搬回开流线程（GUI）。
    // 不做这一步，重开同一通道必然哑火：上一轮的 QThread 析构后，本对象的线程
    // 归属仍挂在那条已不存在的线程上，下一次 start() 里 moveToThread() 会被 Qt
    // 拒绝（"Current thread is not the object's thread"），于是 started→decodeLoop
    // 这条队列连接永远投递不到，本路一帧都不出。
    // finished 由正在结束的线程自己发出（DirectConnection 保证在本线程执行），
    // 此刻 currentThread 正是对象所属线程，移动才合法。
    // 两个判据各挡一种误伤：liveLoops_ 挡"本对象已被新一轮 start() 接管"，
    // currentThread()==thread() 挡"被遗弃的旧线程比新一轮 start() 更晚退出"。
    QThread *owner = QThread::currentThread();
    connect(thread_, &QThread::finished, this, [this, owner] {
        if (liveLoops_.load() == 0 && QThread::currentThread() == thread())
            moveToThread(owner);
    }, Qt::DirectConnection);

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

    // 帧捕获槽先置空：消费者（智能识别页）此后取到 nullptr 即停手，
    // 不会再摸到下面即将释放的 dst_bufs。
    capturePtr_.store(nullptr, std::memory_order_release);

    // 1) 先请求所有推理任务退出（关队列唤醒，不阻塞）
    for (PpeTask *task : tasks_) {
        task->requestStop();
    }

    // 2) 停解码线程（interrupt_callback 中断 av_read_frame，处理完当前帧即退出）
    //    绝不调用 terminate()：强杀可能落在 FFmpeg/MPP/驱动自旋锁内部，
    //    留下互斥量永久上锁 / 堆损坏（未定义行为）。协作式停止已覆盖所有
    //    阻塞点（IO 走 interrupt 回调、队列已 close、循环条件查 running_），
    //    超时只说明卡在驱动级阻塞，多等是正解，强杀是引入随机崩溃。
    if (thread_)
    {
        thread_->quit();   // 退出该线程事件循环（decodeLoop 返回后生效）
        if (!thread_->wait(3000)) {
            qWarning() << "Decoder thread still alive after 3s, waiting cooperative exit";
            if (!thread_->wait(10000)) {
                qCritical() << "Decoder thread did not exit in 13s; abandoning it"
                               " (no terminate: kill inside FFmpeg/driver locks is UB)";
                // 遗弃而非强杀：QThread 退出后经 finished→deleteLater 自行回收；
                // start() 通过 liveLoops_ 拒绝在旧循环存活期间重启。
            }
        }
        thread_ = nullptr;
    }

    // 3) 有界回收推理线程：最多等 1.5s，超时 detach，绝不阻塞 UI 线程
    //    审查点：tasks_ 只 stop 从不 delete——析构函数同样只调 stop()。
    //    这是有意为之：detach 后的推理线程仍可能访问 this/队列，delete 会
    //    造成 use-after-free；代价是每路解码器泄漏至多 5 个 PpeTask 对象
    //    （通道数固定、进程生命周期内不重复创建，总量可控）。
    //    running_ 不再在此复位：若上面的解码线程被遗弃，它仍在看 running_，
    //    复位会复活僵尸循环；start() 负责置 true。
    for (PpeTask *task : tasks_) {
        task->stopBestEffort(1500);
    }
}

// ============================================================================
// signalStop: 仅发送停止信号，不等待线程退出
// ============================================================================
// 用于多路解码器并行停止：先对所有解码器调用 signalStop()，
// 再统一调用 waitForStop()，避免串行等待导致关闭时间叠加。
void FFmpegVideoDecoder::signalStop()
{
    running_ = false;
    for (PpeTask *task : tasks_) {
        task->requestStop();
    }
    if (thread_) {
        thread_->quit();
    }
}

// ============================================================================
// waitForStop: 等待解码线程和推理线程退出
// ============================================================================
// 前提：已调用 signalStop() 或 stop()。
void FFmpegVideoDecoder::waitForStop()
{
    if (thread_)
    {
        if (!thread_->wait(3000)) {
            qWarning() << "Decoder thread still alive after 3s, waiting cooperative exit";
            if (!thread_->wait(10000)) {
                qCritical() << "Decoder thread did not exit in 13s; abandoning it";
            }
        }
        thread_ = nullptr;
    }
    for (PpeTask *task : tasks_) {
        task->stopBestEffort(1500);
    }
}

// ============================================================================
// setRecordingConfig: 注入录制参数（不在此分配资源）
// ============================================================================
void FFmpegVideoDecoder::setRecordingConfig(const RecorderConfig &cfg)
{
    recordingCfg_ = cfg;
    qInfo() << "Video recording config:" << (cfg.enabled ? "enabled" : "disabled")
            << "buffer" << cfg.bufferSeconds << "s codec" << cfg.codec
            << "bitrate" << cfg.bitRate << "gop" << cfg.gopSeconds << "s";
    // 环形缓冲区必须装得下整个报警窗口，否则"前 3 秒"会被新包覆盖而截断
    if (qint64(cfg.bufferSeconds) * 1000 < kClipPreMs + kClipPostMs) {
        qWarning() << "recording.buffer_seconds" << cfg.bufferSeconds
                   << "< clip window" << (kClipPreMs + kClipPostMs) / 1000
                   << "s — 报警片段前向会被截断";
    }
}

bool FFmpegVideoDecoder::getCurrentFrameInfo(int &width, int &height, int &stride) const
{
    // 三个值分三次 relaxed 读，理论上不原子；但 dst_bufs 的尺寸在整个流生命周期内
    // 固定（vid_w/vid_h 打开流时定死），所以不会出现"新宽配旧 stride"的组合。
    width = captureW_.load(std::memory_order_relaxed);
    height = captureH_.load(std::memory_order_relaxed);
    stride = captureStride_.load(std::memory_order_relaxed);
    return width > 0 && height > 0;
}

const uint8_t* FFmpegVideoDecoder::captureCurrentFramePtr() const
{
    return capturePtr_.load(std::memory_order_acquire);
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
    // 存活计数 RAII：覆盖本函数所有 return 路径，供 start() 判断旧循环是否已退场
    struct LiveGuard {
        std::atomic<int> &c;
        explicit LiveGuard(std::atomic<int> &cnt) : c(cnt) { ++c; }
        ~LiveGuard() { --c; }
    } liveGuard{liveLoops_};

    int ret;

    // ===== 步骤1: 打开视频流 =====
    // AVFormatContext 是 FFmpeg 的核心结构，管理输入流的所有信息
    AVFormatContext *fmt_ctx = nullptr;

    // 设置中断回调：当用户调用 stop() 将 running_ 设为 false 时，
    // av_read_frame() 和 avcodec_receive_frame() 会立即返回错误，
    // 使解码线程能快速退出，避免阻塞在 IO 上（尤其是网络流）
    AVIOInterruptCB interrupt_cb = {decodeInterruptCallback, &running_};
    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        qCritical() << "avformat_alloc_context failed (OOM)";
        emit error("Decoder init failed (OOM)");
        return;
    }
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
    if ((ret = avformat_find_stream_info(fmt_ctx, nullptr)) < 0) {
        // 断流/被 interrupt 打断时可能拿不到完整 codecpar；此处不再依赖兜底，
        // 直接走错误分支释放资源（find_best_stream 基于坏 codecpar 的结果不可信）
        qWarning() << "avformat_find_stream_info failed, ret =" << ret;
        emit error("Cannot read stream info");
        avformat_close_input(&fmt_ctx);
        return;
    }

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
    // 根据流的 codec_id 选择合适的硬件解码器，找不到则退回软解
    enum AVCodecID codec_id = fmt_ctx->streams[vidx]->codecpar->codec_id;
    const AVCodec *codec = nullptr;

    // 按 codec_id 选择对应的 rkmpp 硬件解码器
    if (codec_id == AV_CODEC_ID_H264) {
        codec = avcodec_find_decoder_by_name("h264_rkmpp");
    } else if (codec_id == AV_CODEC_ID_HEVC) {
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
    }

    // 硬件解码器找不到，退回通用软解码
    if (!codec) {
        codec = avcodec_find_decoder(codec_id);
    }
    if (!codec)
    {
        emit error("No decoder");
        avformat_close_input(&fmt_ctx);
        return;
    }

    // 创建解码器上下文，用于存放解码器的状态和配置
    AVCodecContext *dec_ctx = avcodec_alloc_context3(codec);
    if (!dec_ctx) {
        qCritical() << "avcodec_alloc_context3 failed (OOM)";
        emit error("Decoder init failed (OOM)");
        avformat_close_input(&fmt_ctx);
        return;
    }

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
    // 释放本地引用（dec_ctx->hw_device_ctx 持有自己的引用，avcodec_free_context 会释放）
    av_buffer_unref(&pHWDeviceCtx);

    // thread_count=1: 硬件解码器本身是单线程的，多线程反而会增加开销
    dec_ctx->thread_count = 1;

    // 打开解码器：此时解码器会初始化 MPP 硬件
    // 打开失败必须终止：继续跑 receive_frame 恒为错误，表现为"无画面但不崩溃"，
    // 不如立即报错释放（此前该返回值未检查）。
    if ((ret = avcodec_open2(dec_ctx, codec, nullptr)) < 0) {
        qCritical() << "avcodec_open2 failed, ret =" << ret;
        emit error("Cannot open decoder");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }

    // 从解码器上下文获取视频分辨率（可能与流信息中的略有不同）
    int vid_w = dec_ctx->width;
    int vid_h = dec_ctx->height;
    qDebug() << "Decoder:" << codec->name << vid_w << "x" << vid_h;

    // 初始化视频录制器（报警片段环形缓冲，如果已启用）
    // 分辨率要用解码器实际输出的宽高，帧率要用流的平均帧率：
    // 编码器的 time_base=1/fps，帧率填错会让 dump 出来的片段时长失真。
    if (recordingCfg_.enabled) {
        RecorderConfig rc = recordingCfg_;
        AVRational fr = fmt_ctx->streams[vidx]->avg_frame_rate;
        if (fr.den == 0 || fr.num == 0) {
            fr = fmt_ctx->streams[vidx]->r_frame_rate;
        }
        if (fr.den != 0 && fr.num != 0) {
            rc.fps = qBound(1, qRound(av_q2d(fr)), 120);
        }
        if (!videoRecorder_) {
            // shared_ptr：ClipWriter 的延迟 dump 任务各持一份引用，
            // 本对象析构/线程停止都不会让后台任务摸到已释放的编码器
            videoRecorder_ = std::make_shared<VideoRecorder>(rc);
        }
        if (!videoRecorder_->init(vid_w, vid_h)) {
            // 硬编不可用只关掉录制，检测/显示主流程照常
            qWarning() << "Video recorder unavailable, recording disabled for ch" << channel_;
            videoRecorder_.reset();
        }
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
    
    dst_bufs[1] = new DmaFrameBuffer();
    dst_bufs[1]->setWidth(vid_w);
    dst_bufs[1]->setHeight(vid_h);
    dst_bufs[1]->setFormat(DRM_FORMAT_RGBA8888);

    // alloc() 返回 DMA-BUF fd 或 -1：失败多为 DRM 设备不可用/显存不足，
    // 继续跑只会让每帧 RGA 转换拿到无效缓冲，必须在此终止
    if (dst_bufs[0]->alloc() < 0 || dst_bufs[1]->alloc() < 0) {
        qCritical() << "DMA-BUF alloc failed for RGBA double buffer";
        emit error("DMA buffer alloc failed");
        emit statusChanged(channel_, 0);   // 已上报在线，失败路径必须复位
        delete dst_bufs[0];
        delete dst_bufs[1];
        close(drm_fd);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }


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
    if (!frame || !pkt) {
        qCritical() << "av_frame_alloc/av_packet_alloc failed (OOM)";
        emit error("Decoder init failed (OOM)");
        emit statusChanged(channel_, 0);
        for (int i = 0; i < 2; i++)
            delete dst_bufs[i];
        close(drm_fd);
        av_frame_free(&frame);
        av_packet_free(&pkt);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return;
    }
    int frame_count = 0;
    boxSmoother_.clear();
    fenceTracks_.clear();   // 重开流即新的一段监控，围栏事件不跨重启延续

    TIMER t;

    // 解码线程分阶段计时（IDGE_PERF=1 才输出）。这条链是串行的：取解码帧 →
    // RGA 转 RGBA → 每 3 帧投递推理 → 合帧画框/围栏/报警 → emit → 节拍睡眠，
    // 帧率上限就由它一个周期的总耗时决定，所以先量清每段占多少微秒再谈优化。
    // recv 段是"上一帧收尾到本帧进入循环体"，即 avcodec_send/receive 的等待，
    // 只有把它算进来，total 才等于真实帧周期。每 120 帧出一个窗口均值。
    //
    // 4 路 1080p/30fps 修复前实测（us/帧，稳态）：recv 0.8~1.8k、rga 1.4~1.7k、
    // infer 0.6k、slot 52k~65k（其中 draw 占 99.8%，pop 15us、alarm <250us）、
    // emit 20~40、total 55k~69k → 实际 14.5~18fps，节拍器 sleep 只有 12us，
    // 说明产帧侧才是瓶颈，且瓶颈就是叠加绘制那一段（draw_text_c4 在非缓存
    // DRM dumb buffer 上逐像素读-改-写做 alpha 混合）。
    //
    // 改纯写绘制 + 去掉重复推理槽位后（us/帧，稳态）：draw 3.1k~4.5k、
    // total 33.3k~34.6k → 4 路全部 29.5~30.0fps，sleep 25k~27k（节拍器重新生效、
    // 有余量），进程 CPU 从 ~445% 降到 ~120%。rga 段拆开看是 import 0.3~0.7k、
    // cvt 0.8~1.1k——每帧 import/release 只占 1.7ms 里的 0.5ms，缓存句柄不值得。
    struct DecodeStages {
        QElapsedTimer frame;   // 一个完整周期（含 recv）
        QElapsedTimer stage;   // 当前阶段
        qint64 recv = 0, rga1 = 0, infer = 0, slot = 0, em = 0, sleep = 0, total = 0;
        qint64 sPop = 0, sDraw = 0, sAlarm = 0;
        qint64 rgaImp = 0, rgaCvt = 0;   // RGA 段内部拆分：导入句柄 vs 真正转换
        int n = 0;
        // 每段结束时 add 并立刻 restart，避免某个分支被跳过时把时间记到下一段头上
        void add(qint64 &acc) { acc += stage.nsecsElapsed() / 1000; }
        void report(int channel) {
            // 累加器必须在"出窗口"时才清零。写在 if 外面等于每帧清零，
            // n 永远到不了阈值，探针一行都不会打印（本轮踩过）。
            if (n < 120 || !perfProbe()) return;
            const double us_per_frame = double(total) / n;
            qWarning() << "[PROBE] ch" << channel << "us/frame"
                       << "recv" << recv / n
                       << "rga" << rga1 / n << "[import" << rgaImp / n << "cvt" << rgaCvt / n << "]"
                       << "infer" << infer / n
                       << "slot" << (sPop + sDraw + sAlarm) / n
                       << "[pop" << sPop / n << "draw" << sDraw / n << "alarm" << sAlarm / n << "]"
                       << "emit" << em / n << "sleep" << sleep / n
                       << "total" << total / n
                       << "fps" << (us_per_frame > 0 ? 1000000.0 / us_per_frame : 0.0);
            recv = rga1 = infer = slot = em = sleep = total = 0;
            sPop = sDraw = sAlarm = 0;
            rgaImp = rgaCvt = 0;
            n = 0;
        }
    } ds;
    ds.frame.start();
    ds.stage.start();
    // 断点标记：每 300 帧在链路的 6 个位置各打一行。若只看到 A 而看不到 F，
    // 说明卡在中间某段；配合 A 的 ms 还能算出真实出帧周期。
    auto mark = [&](const char *tag) {
        if (perfProbe() && frame_count % 300 == 0)
            qWarning() << "[TRACE] ch" << channel_ << frame_count << tag;
    };

    // ===== 帧率节拍（截止期式，限"产生"速率）=====
    // 用流自身帧率定 target_fps；循环末尾对每个解码帧"睡到发布点 / 落后重锚"（见 4.12）。
    // 关键：限的是解码产生速率，不只是显示——否则解码/推理/RGA 会被超量喂爆（实测卡死根因）。
    // 落后时重锚到 now，绝不"睡到累计目标线去补追赶连发"（那正是 1↔50 的来源）。
    int target_fps = 25;
    {
        AVRational pfr = fmt_ctx->streams[vidx]->avg_frame_rate;
        if (pfr.den == 0 || pfr.num == 0) {
            pfr = fmt_ctx->streams[vidx]->r_frame_rate;
        }
        if (pfr.den != 0 && pfr.num != 0) {
            target_fps = qBound(1, qRound(av_q2d(pfr)), 120);
        }
    }
    // 上限 30fps：RK3588 单路解码+推理能力约 30fps，
    // 流元数据常报 50/90/120fps（摄像头默认值），超过硬件能力
    // 会导致每帧都落后、累计线永远追不上 → 帧率振荡。
    if (target_fps > 30) target_fps = 30;
    const qint64 publish_interval_us = 1000000 / target_fps;
    QElapsedTimer pub_clock;
    pub_clock.start();
    // 锚线整体偏移量：只在"落后超过一个周期"时更新（见 4.12 的重锚分支）。
    // 纯累计线（offset 恒 0）是开环的——一次抖动后 expected 永远落后于 elapsed，
    // while 条件不再成立，节拍器从此形同虚设。
    qint64 pacer_drift_us = 0;

    // ===== 节拍目标帧率识别（每路开流打印一次，便于核对源帧率口径）=====
    qInfo() << "decoder pacer ch" << channel_ << "target_fps=" << target_fps
            << "(avg_fps=" << av_q2d(fmt_ctx->streams[vidx]->avg_frame_rate)
            << " r_fps=" << av_q2d(fmt_ctx->streams[vidx]->r_frame_rate) << ")";

    // 软件帧转换上下文缓存：源格式/宽高在一路流内恒定，故只在建流后首次遇到
    // 软件帧时建一次（见 4.5 的软件分支）。三个变量都是本线程私有的。
    SwsContext* software_scaler = nullptr;
    int software_scaler_src = AV_PIX_FMT_NONE;
    int software_scaler_w = 0, software_scaler_h = 0;

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
            mark("A enter");
            ds.add(ds.recv);
            ds.stage.restart();

            // 不在这里丢帧：解码帧率由循环末尾的"睡眠到点 + 落后重锚"节拍器整体限住
            // （见 4.12）。若在此丢弃超前的解码帧，解码线程仍会全速把源抽到 ~250fps，
            // 而推理(frame_count%3)、MPP 解码、RGA 缩放都跟着解码帧率走 → 会被超量喂爆
            // （实测 RGA_COLORFILL 每帧失败刷屏 + 卡死）。限住"产生"才是根治。

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

            if (src_fd >= 0 || (src_fd < 0 && frame->data[0] &&
                                frame->format != AV_PIX_FMT_DRM_PRIME))
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
                long long rgaImpNs = 0, rgaCvtNs = 0;
                long long *pImp = perfProbe() ? &rgaImpNs : nullptr;
                long long *pCvt = perfProbe() ? &rgaCvtNs : nullptr;
                bool ok = false;
                if (src_fd >= 0) {
                    ok = RgaConverter::convertNV12ToRGBAbyRGA(
                        src_fd, 
                        dst_buf->fd(), 
                        frame->width, 
                        frame->height, 
                        src_stride, 
                        frame->height,
                        RK_FORMAT_YCbCr_420_SP,
                        pImp, pCvt);
                } else {
                    // CPU 内存帧（v4l2 摄像头直出 NV12/YUYV 裸流、或没有硬解路径的
                    // 软解流）：本函数原本只认 DRM_PRIME，这类帧会被整帧丢弃，表现为
                    // "摄像头明明有流、智能识别却取不到画面"。这里用 sws 转成 RGBA 写进
                    // 同一块 DMA 缓冲，下游画框/推理/EGL 与硬解路径共用一套代码。
                    if (!software_scaler || software_scaler_src != frame->format ||
                        software_scaler_w != frame->width || software_scaler_h != frame->height) {
                        sws_freeContext(software_scaler);
                        software_scaler = sws_getContext(
                            frame->width, frame->height,
                            static_cast<AVPixelFormat>(frame->format),
                            vid_w, vid_h, AV_PIX_FMT_RGBA,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                        software_scaler_src = frame->format;
                        software_scaler_w = frame->width;
                        software_scaler_h = frame->height;
                    }
                    if (software_scaler) {
                        // CPU 即将写这块 GPU 也在读的缓冲：先收回所有权，写完再交还
                        dma_sync_device_to_cpu(dst_buf->fd());
                        uint8_t* dst_data[4] = { dst_buf->ptr(), nullptr, nullptr, nullptr };
                        int dst_linesize[4] = { dst_buf->stride(), 0, 0, 0 };
                        ok = sws_scale(software_scaler, frame->data, frame->linesize, 0,
                                       frame->height, dst_data, dst_linesize) > 0;
                        if (ok && dma_sync_cpu_to_device(dst_buf->fd()) < 0) {
                            msync(dst_buf->ptr(), dst_buf->size(), MS_SYNC);
                        }
                    }
                }
                ds.rgaImp += rgaImpNs / 1000;
                ds.rgaCvt += rgaCvtNs / 1000;
                ds.add(ds.rga1);
                ds.stage.restart();
                mark("B rga");

                if (ok)
                {
                    // ===== 4.6 每 3 帧做一次推理（降频处理） =====
                    //
                    // 为什么不是每帧都推理？：
                    //   - 视频通常是 25-30 fps，但推理速度可能只有 10-15 fps
                    //   - 每 3 帧推理一次 = 约 8-10 fps 的检测频率
                    //   - 足够用于实时告警，同时留出算力给其他通道
                    //
                    if (inferenceEnabled_ && frame_count % 3==0) {
                        ds.stage.restart();
                        
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
                        ds.add(ds.infer);
                        ds.stage.restart();
                        mark("C infer");
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
                    ds.stage.restart();
                    image_buffer_t dislayImage;
                    dislayImage.format = image_format_t::IMAGE_FORMAT_RGBA8888;
                    dislayImage.virt_addr = (unsigned char*)dst_buf->ptr();
                    dislayImage.width = dst_buf->width();
                    dislayImage.height = dst_buf->height();

                    // ===== 4.9b 报警上下文与围栏候选（本帧一次） =====
                    //
                    // 每槽位的报警上下文：该模型的类名表 + 报警类别（"模型的参数"，
                    // 来自 config 的 video.models[<库id>].alarm_class）。
                    // 由解码器构造、随结果传进 AlarmManager，单例里不再存全局槽表——
                    // 按通道配置之后，通道 0 的槽位 1 与通道 2 的槽位 1 是两个不同模型。
                    std::vector<AlarmManager::SlotContext> slotCtxs(slotQueues_.size());
                    // 报警类别每帧现取一份快照：设置页改 alarm_class 时不重启、不重开本路
                    // 也能生效（槽位数不变的前提下）。锁只覆盖这一次 ≤5 元素的拷贝。
                    std::vector<QString> alarmClassSnap;
                    {
                        std::lock_guard<std::mutex> lk(alarmClsMtx_);
                        alarmClassSnap = slotAlarmClasses_;
                    }
                    for (size_t k = 0; k < slotQueues_.size(); ++k) {
                        slotCtxs[k].alarmClass =
                            (k < alarmClassSnap.size()) ? alarmClassSnap[k] : QString();
                        slotCtxs[k].classNames.reserve((int)slotClassNames_[k].size());
                        for (const auto &n : slotClassNames_[k])
                            slotCtxs[k].classNames << QString::fromStdString(n);
                    }

                    // cls_id → 类别名，与 AlarmManager::ingest 内部同口径（越界记 cls_N）
                    auto slotClassNameAt = [](const QStringList &names, int clsId) {
                        return (clsId >= 0 && clsId < names.size())
                                   ? names[clsId]
                                   : QString("cls_%1").arg(clsId);
                    };

                    // 围栏只按"通道"配置：类别是 geofence.alarmClass 单值（默认 person），
                    // 与本通道挂了几个模型、每个模型的报警类别都无关。
                    auto &fenceMgr = geofence::FenceManager::instance();
                    const bool fenceScan = fenceMgr.enabled() && fenceMgr.hasFence(channel_);
                    const QString fenceCls = fenceMgr.alarmClass();

                    struct FenceCandidate {
                        object_detect_result det;
                        size_t slot = 0;      // 检出它的槽位，用于取回该类名表与节流口径
                        QString className;
                        long timeNs = 0;      // 该结果的采集时刻（epoch 纳秒）
                    };
                    std::vector<FenceCandidate> fenceCandidates;

                    for (size_t k = 0; k < slotQueues_.size(); ++k) {
                        ds.stage.restart();
                        // 从该模型的结果队列中取最新一次检测结果
                        // tryPop: 非阻塞，无新结果时 od 保持空（count=0）
                        object_detect_result_list od;
                        memset(&od, 0, sizeof(od));
                        bool hasNewResult = slotQueues_[k]->tryPop(od);

                        // ===== 检测框时序平滑 =====
                        // 每 3 帧才推理一次，直接画原始坐标会导致框"跳"。
                        // 用指数平滑让显示坐标逐帧趋近检测目标，视觉连续。
                        // 仅影响绘制；下方围栏/报警仍用 od 原始坐标。
                        while (boxSmoother_.size() < slotQueues_.size())
                            boxSmoother_.emplace_back();
                        auto &smoother = boxSmoother_[k];

                        // 有新检测结果时才更新 track（匹配 → 插值 → 新增 → 淡出）
                        // 无新结果时保持现有 track 不变，继续画平滑位置
                        if (hasNewResult) {
                            const float iouThr = 0.2f;

                            if (od.count > 0) {
                                for (int i = 0; i < od.count; ++i) {
                                    const auto &det = od.results[i];
                                    float nx = (float)det.box.left;
                                    float ny = (float)det.box.top;
                                    float nw = (float)(det.box.right - det.box.left);
                                    float nh = (float)(det.box.bottom - det.box.top);

                                    int bestIdx = -1;
                                    float bestIou = iouThr;
                                    for (size_t t = 0; t < smoother.tracks.size(); ++t) {
                                        auto &tr = smoother.tracks[t];
                                        if (tr.cls_id != det.cls_id || tr.fade > 0) continue;
                                        float ix1 = std::max(tr.dx, nx);
                                        float iy1 = std::max(tr.dy, ny);
                                        float ix2 = std::min(tr.dx + tr.dw, nx + nw);
                                        float iy2 = std::min(tr.dy + tr.dh, ny + nh);
                                        float iw = std::max(0.0f, ix2 - ix1);
                                        float ih = std::max(0.0f, iy2 - iy1);
                                        float inter = iw * ih;
                                        float iou = inter / (tr.dw * tr.dh + nw * nh - inter + 1e-6f);
                                        if (iou > bestIou) { bestIou = iou; bestIdx = (int)t; }
                                    }

                                    if (bestIdx >= 0) {
                                        auto &tr = smoother.tracks[bestIdx];
                                        tr.tx = nx; tr.ty = ny; tr.tw = nw; tr.th = nh;
                                        tr.prop = det.prop;
                                    } else {
                                        BoxTrack nt;
                                        nt.dx = nt.tx = nx;
                                        nt.dy = nt.ty = ny;
                                        nt.dw = nt.tw = nw;
                                        nt.dh = nt.th = nh;
                                        nt.cls_id = det.cls_id;
                                        nt.prop = det.prop;
                                        smoother.tracks.push_back(nt);
                                    }
                                }

                                // 未匹配的活跃 track 开始淡出
                                for (auto &tr : smoother.tracks) {
                                    if (tr.fade > 0) continue;
                                    bool matched = false;
                                    for (int i = 0; i < od.count; ++i) {
                                        if (tr.cls_id != od.results[i].cls_id) continue;
                                        float nx = (float)od.results[i].box.left;
                                        float ny = (float)od.results[i].box.top;
                                        float nw = (float)(od.results[i].box.right - od.results[i].box.left);
                                        float nh = (float)(od.results[i].box.bottom - od.results[i].box.top);
                                        float ix1 = std::max(tr.dx, nx);
                                        float iy1 = std::max(tr.dy, ny);
                                        float ix2 = std::min(tr.dx + tr.dw, nx + nw);
                                        float iy2 = std::min(tr.dy + tr.dh, ny + nh);
                                        float iw = std::max(0.0f, ix2 - ix1);
                                        float ih = std::max(0.0f, iy2 - iy1);
                                        float inter = iw * ih;
                                        float iou = inter / (tr.dw * tr.dh + nw * nh - inter + 1e-6f);
                                        if (iou > iouThr) { matched = true; break; }
                                    }
                                    if (!matched) tr.fade = 6;
                                }
                            } else {
                                // 新检测结果为空：所有活跃 track 开始淡出
                                for (auto &tr : smoother.tracks)
                                    if (tr.fade == 0) tr.fade = 6;
                            }
                        }

                        // 指数平滑 + 淡出递减 + 清除已消失 track
                        for (auto it = smoother.tracks.begin(); it != smoother.tracks.end(); ) {
                            if (it->fade > 0 && --it->fade <= 0) {
                                it = smoother.tracks.erase(it);
                                continue;
                            }
                            float a = (it->fade > 0) ? 0.3f : 0.6f;
                            it->dx += a * (it->tx - it->dx);
                            it->dy += a * (it->ty - it->dy);
                            it->dw += a * (it->tw - it->dw);
                            it->dh += a * (it->th - it->dh);
                            ++it;
                        }

                        // 画平滑后的框
                        ds.add(ds.sPop); ds.stage.restart();
                        int color = kSlotColors[(int)k % 5];
                        char text[256];
                        for (const auto &tr : smoother.tracks) {
                            int dl = (int)std::round(tr.dx);
                            int dt = (int)std::round(tr.dy);
                            int dw = (int)std::round(tr.dw);
                            int dh = (int)std::round(tr.dh);
                            if (!noOverlayDraw())
                                draw_rectangle(&dislayImage, dl, dt, dw, dh, color, 3);

                            const std::vector<std::string> &slotNames = slotClassNames_[k];
                            if (tr.cls_id >= 0 && tr.cls_id < (int)slotNames.size())
                                snprintf(text, sizeof(text), "M%zu %s %.1f%%",
                                         k + 1, slotNames[tr.cls_id].c_str(), tr.prop * 100);
                            else
                                snprintf(text, sizeof(text), "M%zu cls_%d %.1f%%",
                                         k + 1, tr.cls_id, tr.prop * 100);
                            if (!noOverlayDraw())
                                draw_text(&dislayImage, text, dl, dt - 20, color, 10);
                        }

                        ds.add(ds.sDraw); ds.stage.restart();
                        // ===== 4.10 本槽位的普通检测报警 =====
                        // 仅在有新检测结果时执行（非推理帧跳过，避免空结果误触发报警）
                        if (hasNewResult && od.count > 0) {
                            QVector<AlarmRecord> newAlarms = AlarmManager::instance()
                                    .ingest(channel_, od, slotCtxs[k]);
                            // 收尾（截图 → 入库通知 → 片段）见 deliverAlarms
                            deliverAlarms(channel_, newAlarms, dislayImage, videoRecorder_);

                            // 围栏候选：围栏是"通道"级判定，不属于任何槽位。
                            // 这里只按类别名把候选收齐，几何判定与报警留到循环之后合并做一次，
                            // 于是围栏通道里的 un_helmet 之类普通报警不再被围栏分支吞掉。
                            if (fenceScan) {
                                for (int i = 0; i < od.count; i++) {
                                    const auto &d = od.results[i];
                                    const QString clsName =
                                        slotClassNameAt(slotCtxs[k].classNames, d.cls_id);
                                    if (clsName != fenceCls)
                                        continue;
                                    FenceCandidate c;
                                    c.det = d;
                                    c.slot = k;
                                    c.className = clsName;
                                    c.timeNs = od.time;
                                    fenceCandidates.push_back(c);
                                }
                            }
                        }
                        ds.add(ds.sAlarm);
                    }

                    // ===== 4.10b 电子围栏：通道级合并判定 + 进入沿触发报警 =====
                    //
                    // 判定放在这里（槽位循环之后、emit frameReady 之前）：
                    //   - 候选已跨槽位收齐，挂 PPE 还是挂 COCO 都能检出 person；
                    //   - 围栏坐标是 overlay 像素空间，需与本帧 dislayImage 尺寸同口径；
                    //   - 截图要用同一帧带框画面，deliverAlarms 才能拍到越界目标。
                    //
                    // 语义是"进入沿触发"而非"电平触发"：一个人站在围栏里只报一次，
                    // 不是每 10 秒报一次。靠 fenceTracks_ 把连续命中同一个目标的若干帧
                    // 归并成"同一次入侵事件"；离开超过 kFenceTrackKeepNs 后事件结束，
                    // 再进来算新事件。（此前每命中一帧就调一次报警，40 秒能出三百多条。）
                    if (fenceScan) {
                        const long long steadyNow =
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();

                        // 淘汰"人已经离开"的旧事件。本轮没有候选（非推理帧、或人全走了）
                        // 也要执行，否则离开者的 track 会一直挂着、回来时不再报警。
                        fenceTracks_.erase(
                            std::remove_if(fenceTracks_.begin(), fenceTracks_.end(),
                                           [steadyNow](const FenceInsideTrack &t) {
                                               return steadyNow - t.lastHitNs > kFenceTrackKeepNs;
                                           }),
                            fenceTracks_.end());

                        if (!fenceCandidates.empty()) {
                            const geofence::ChannelFence cf = fenceMgr.channelFence(channel_);
                            // alarmInside=false(outside_alarm) 时"命中"=在围栏外，
                            // 下面逻辑于是变成"越界沿触发"，语义同样成立
                            const bool alarmInside = (fenceMgr.mode() == "inside_alarm");
                            int oW = 0, oH = 0;
                            fenceMgr.overlaySize(channel_, oW, oH);

                            QVector<AlarmRecord> fenceAlarms;
                            int insideCount = 0;   // 命中围栏的目标数（含同一次入侵的续期）
                            int entryCount = 0;    // 其中判定为"新进入"的次数 = 建事件数
                            for (const FenceCandidate &c : fenceCandidates) {
                                // 脚点（框底边中点）是否落在围栏多边形内
                                const bool inside = geofence::FenceChecker::checkDetection(
                                        c.det,
                                        dislayImage.width, dislayImage.height,
                                        640, 640, oW, oH,
                                        cf, alarmInside);
                                if (!inside)
                                    continue;
                                insideCount++;

                                const float nx = (float)c.det.box.left;
                                const float ny = (float)c.det.box.top;
                                const float nw = (float)(c.det.box.right - c.det.box.left);
                                const float nh = (float)(c.det.box.bottom - c.det.box.top);

                                // 与进行中的入侵事件做 IoU 匹配：匹配上 = 同一个人还在栏内，
                                // 只把框与时刻刷成新值（事件续期），不报警
                                int bestIdx = -1;
                                float bestIou = kFenceTrackIouThr;
                                for (size_t t = 0; t < fenceTracks_.size(); ++t) {
                                    const auto &tr = fenceTracks_[t];
                                    const float ix1 = std::max(tr.x, nx);
                                    const float iy1 = std::max(tr.y, ny);
                                    const float ix2 = std::min(tr.x + tr.w, nx + nw);
                                    const float iy2 = std::min(tr.y + tr.h, ny + nh);
                                    const float inter =
                                        std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
                                    const float iou =
                                        inter / (tr.w * tr.h + nw * nh - inter + 1e-6f);
                                    if (iou > bestIou) { bestIou = iou; bestIdx = (int)t; }
                                }
                                if (bestIdx >= 0) {
                                    auto &tr = fenceTracks_[bestIdx];
                                    tr.x = nx; tr.y = ny; tr.w = nw; tr.h = nh;
                                    tr.lastHitNs = steadyNow;
                                    continue;
                                }

                                // 匹配不上 = 进入沿：报一次警，并建立事件
                                entryCount++;
                                fenceAlarms += AlarmManager::instance().ingestFenceAlarm(
                                        channel_, c.det, slotCtxs[c.slot], c.timeNs);
                                FenceInsideTrack tr;
                                tr.x = nx; tr.y = ny; tr.w = nw; tr.h = nh;
                                tr.lastHitNs = steadyNow;
                                fenceTracks_.push_back(tr);
                            }

                            QStringList alarmClassDesc;
                            for (size_t k = 0; k < slotCtxs.size(); ++k)
                                alarmClassDesc << QString("M%1=%2").arg(k + 1)
                                        .arg(slotCtxs[k].alarmClass.isEmpty()
                                                 ? QStringLiteral("(不报警)")
                                                 : slotCtxs[k].alarmClass);
                            // 报警条数可能少于"新进入"数：同一通道同一类别被
                            // geofence.throttleSeconds 兜住了（进入沿抖动/两人几乎同时进入），
                            // 这两个数并排打出来就是为了能看出来被兜住了。
                            fenceMgr.postLog("fence",
                                QString("[通道%1] 围栏: 模式=%2, 围栏类别=%3, 围栏=%4个, "
                                        "本通道报警类=[%5] | 候选%6个, 栏内%7个, 新进入%8个, "
                                        "报警%9条(最小间隔%10秒)")
                                    .arg(channel_ + 1).arg(fenceMgr.mode()).arg(fenceCls)
                                    .arg(cf.shapes.size()).arg(alarmClassDesc.join(", "))
                                    .arg((int)fenceCandidates.size()).arg(insideCount)
                                    .arg(entryCount).arg(fenceAlarms.size())
                                    .arg(ConfigManager::instance().geofenceThrottleSeconds()));

                            if (!fenceAlarms.isEmpty()) {
                                fenceMgr.postLog("alarm",
                                    QString("  [通道%1] ★ 围栏侵入报警: %2 (置信度 %3%)")
                                        .arg(channel_ + 1)
                                        .arg(fenceAlarms.front().className)
                                        .arg(fenceAlarms.front().confidence * 100, 0, 'f', 1));
                            }
                            deliverAlarms(channel_, fenceAlarms, dislayImage, videoRecorder_);
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
                    mark("D slot");
                    RenderFrame rf;
                    rf.fd = dup(dst_buf->fd());
                    rf.width = vid_w;
                    rf.height = vid_h;
                    rf.stride = dst_buf->stride();

                    // 视频录制：把"已画框的 RGBA DMA-BUF"零拷贝编码进环形缓冲区
                    // 报警时可 dump 报警前后各若干秒的带框片段
                    //
                    // 传 fd 而不是 ptr：RGA 在 DMA-BUF 之间搬像素，CPU 不碰像素数据。
                    // stride 口径：DmaFrameBuffer::stride() 是字节 pitch，
                    // RGA 要的是像素跨度，RGBA 为 4 字节/像素 → 除以 4。
                    if (videoRecorder_ && videoRecorder_->isInitialized()) {
                        videoRecorder_->encodeFrame(
                            dst_buf->fd(), vid_w, vid_h,
                            dst_buf->stride() / 4,
                            QDateTime::currentMSecsSinceEpoch());
                    }

                    emit frameReady(rf);
                    // 更新帧捕获槽（供智能识别 QR/OCR 按需取帧）
                    // 开销：三次 relaxed store + 一次 release store，无拷贝、无系统调用。
                    // 放在 emit 之后，保证 GUI 侧先拿到本帧渲染、再让取帧者看到同一帧。
                    // 尺寸对整个流恒定，故每帧重复 store 只是为了让首个消费者尽早拿到几何值。
                    {
                        captureW_.store(vid_w, std::memory_order_relaxed);
                        captureH_.store(vid_h, std::memory_order_relaxed);
                        captureStride_.store(dst_buf->stride(), std::memory_order_relaxed);
                        capturePtr_.store(dst_buf->ptr(), std::memory_order_release);
                    }
                    ds.add(ds.em);
                    ds.stage.restart();
                    mark("E emit");
                   
                }

                // 切换双缓冲：这次写 0 号缓冲，下次写 1 号缓冲
                // 避免 RGA 还在读 0 号时，解码器就开始写 0 号
                back_buf = 1 - back_buf;
            }

            // 释放帧的引用，允许 FFmpeg 重用底层内存
            av_frame_unref(frame);

            // ===== 4.12 解码帧率节拍（累计线 + 落后重锚 + 分片短睡）=====
            //   expected = frame_count × interval + drift  —— 累计锚线
            //   领先时：分片睡到锚线（≤4ms 一片，复查 running_ 保证 stop 及时）
            //   落后一个周期以内：不睡，靠后续帧自然收敛（阻尼，避免密-疏脉冲）
            //   落后超过一个周期：把 drift 挪到"此刻正好对齐"，即重锚
            //
            // 为什么必须重锚：文件源不是实时源，av_read_frame 想取多少帧就有多少帧。
            // 纯累计线落后后再也不睡 → 实测同一份 30fps 素材被追到 50fps 播放
            // （界面 FPS 短暂冲到 36+），MPP/RGA/推理全在超量空转。
            // 为什么阈值取一个周期而不是"一落后就重锚"：后者正是老注释担心的
            // "每帧落后→每帧重置→每帧睡满 interval"脉冲来源；只在落后超过一帧时
            // 重锚，小抖动仍由累计线自然阻尼。
            {
                qint64 expected_us = (qint64)frame_count * publish_interval_us + pacer_drift_us;
                qint64 elapsed_us = pub_clock.nsecsElapsed() / 1000;
                if (elapsed_us - expected_us > publish_interval_us) {
                    pacer_drift_us = elapsed_us - (qint64)frame_count * publish_interval_us;
                    expected_us = elapsed_us;
                }
                while (running_ && expected_us > elapsed_us) {
                    const qint64 rem_us = expected_us - elapsed_us;
                    QThread::usleep(static_cast<unsigned long>(rem_us > 4000 ? 4000 : rem_us));
                    elapsed_us = pub_clock.nsecsElapsed() / 1000;
                }
            }
            mark("F post-pacer");
            ds.add(ds.sleep);
            ds.total += ds.frame.nsecsElapsed() / 1000;
            ++ds.n;
            ds.report(channel_);
            mark("G tail");
            ds.frame.restart();
            ds.stage.restart();

            t.tok();
        }
    }

    qDebug() << "Decode finished, frames:" << frame_count;

    // 软件帧转换上下文（可能从未创建——纯硬解流走不到那个分支，free 容忍 NULL）
    sws_freeContext(software_scaler);
    software_scaler = nullptr;

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
    //   - dec_ctx->hw_device_ctx：av_buffer_ref(pHWDeviceCtx) 创建，
    //     avcodec_free_context 自动释放；本地 pHWDeviceCtx 已 av_buffer_unref。
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
