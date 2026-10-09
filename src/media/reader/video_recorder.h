#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

// ============================================================================
// VideoRecorder - 硬件编码环形缓冲区录制器
// ============================================================================
//
// 作用：
//   把"已经在 RGBA DMA-BUF 上画好检测框"的画面持续编码为 H.265/H.264 Annex-B
//   裸流，存进内存环形缓冲区；报警时按时间窗口 dump 成 .h265/.h264 片段。
//   报警视频必须带框，所以编码源只能是显示侧那块画过框的 RGBA 缓冲，
//   不能重新解码原流。
//
// 全硬件链路（像素全程不过 CPU）：
//   RGBA DMA-BUF(显示侧，已画框)
//     → RGA rgba_to_nv12（零拷贝，fd→fd）
//     → rkmpp hwframe 池帧(AV_PIX_FMT_DRM_PRIME / NV12)
//     → hevc_rkmpp / h264_rkmpp 硬编码（MPP）
//     → AVPacket(Annex-B) 入环形缓冲区
//
// 板端实测（2026-09-29，/tmp/enc_probe）确定的三个事实，本类实现据此而来：
//   1) hevc_rkmpp 接受 AV_PIX_FMT_DRM_PRIME 输入（配 hw_frames_ctx 即可），
//      所以不需要 NV12+mmap 的软拷贝退路；
//   2) 编码器每个 IDR 前都内嵌 VPS/SPS/PPS（21/21 关键帧如此），
//      因此从任意关键帧起 dump 的裸流都能独立解码，dump 侧无需补参数集；
//   3) RGA 的 wstride/hstride 单位是像素/行，不是字节（详见 rga_converter.h）。
//
// 时间戳口径（两套，各司其职）：
//   - wall_ms_：本机毫秒墙钟，**只**用于窗口选取（报警时刻 ± 前后秒数）和落库展示；
//   - steady_pts：单调帧计数折算（encodeIndex_ * 1000/fps，编码器 time_base=1/fps），
//     AVFrame->pts 用它。与墙钟解耦后，到达抖动不再进包时间线，编码包 PTS 严格等距；
//     丢帧时帧计数仍连续，dump 出的片段时长口径与源帧率一致。
//
// 线程模型：
//   encodeFrame() 在解码线程逐帧调用；dump*() 由报警侧的后台线程调用，
//   因此 ringBuffer_ 用 mutex_ 保护。dump 只在持锁期间把包拷出，
//   文件 IO 在锁外做，避免阻塞编码。
//   dumpMp4 只用 init() 时快照下来的 streamParams_，不碰 codecCtx_——
//   否则后台 dump 会与解码线程的 send/receive 并发读写同一编码器上下文。
//
// 生命周期：
//   由 FFmpegVideoDecoder 以 shared_ptr 持有；报警片段是"报警后若干秒"才做的
//   延迟任务，任务里带着同一份 shared_ptr，因此解码器停止/销毁后录制器仍然存活，
//   不会出现后台线程访问已释放的编码器。
//
// ============================================================================

#include <QString>
#include <QVector>
#include <QMutex>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

/**
 * @brief 录制参数（来自 config.json 的 recording 段）
 *
 * @note 这里不含输出目录：片段落在 RuntimePaths::alarmsDir()/yyyyMMdd，
 *       与报警抓拍图同一棵树（成对查找、一套按天清理）。目录拼接由调用方
 *       （FFmpegVideoDecoder 的报警分支）负责，本类只管编码与 dump，
 *       保持与通道/路径等业务无关。
 */
struct RecorderConfig
{
    bool enabled = false;       ///< 总开关：false 时不分配任何硬件资源
    int bufferSeconds = 5;      ///< 环形缓冲区时长（秒）
    int fps = 25;               ///< 名义帧率：编码器 time_base 与 pts 折算基准
    int bitRate = 2000000;      ///< 码率（bps），CBR 模式
    int gopSeconds = 1;         ///< 关键帧间隔（秒），决定片段起点对齐误差上限
    QString codec = "hevc";     ///< "hevc"=hevc_rkmpp（主用）/ "h264"=h264_rkmpp
};

class VideoRecorder
{
public:
    /**
     * @brief 构造函数
     * @param cfg: 录制参数
     *
     * @note 只记录参数，不分配硬件资源；硬件资源在 init() 里建，
     *       因为分辨率要等解码器打开流之后才知道。
     *       编码器上下文/帧池是本对象独占的裸资源，禁止拷贝。
     */
    explicit VideoRecorder(const RecorderConfig& cfg);
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;
    ~VideoRecorder();

    /**
     * @brief 初始化硬件编码器与 DMA 帧池
     * @param width:  视频宽度（画框后的 RGBA 缓冲尺寸）
     * @param height: 视频高度
     * @return: true=可用，false=不可用（调用方应保持录制关闭，不影响检测主流程）
     *
     * @note 编码链路任一步失败（无 rkmpp 设备 / 无 rkmpp 编码器 / 打开失败）
     *       一律返回 false 并 qWarning 说明原因，**不回落 CPU 软编**：
     *       4 路 1080p 的 x264 会把 CPU 吃满，直接拖垮检测与显示，
     *       "宁可不录，也不能为录制牺牲主流程"。
     */
    bool init(int width, int height);

    /**
     * @brief 编码一帧并存入环形缓冲区（零拷贝路径）
     * @param rgba_fd:  画好框的 RGBA DMA-BUF fd
     * @param w/h:      RGBA 帧尺寸（像素）
     * @param rgba_wstride_px: RGBA 行跨度（**像素**，= 字节 pitch / 4）
     * @param wall_ms:  本机毫秒墙钟（该帧的采集/绘制时刻）
     *
     * @note 在解码线程逐帧调用。池帧借不到时先排空编码器再重试一次，
     *       仍失败则丢弃本帧并计数（dropFrames() 可读），绝不阻塞等待。
     */
    void encodeFrame(int rgba_fd, int w, int h, int rgba_wstride_px, qint64 wall_ms);

    /**
     * @brief dump 环形缓冲区窗口为 Annex-B 裸流（上传服务器用）
     * @param filePath: 输出路径（.h265 / .h264）
     * @param center_ms: 窗口中心（通常是报警时刻）
     * @param pre_ms:  向前取多少毫秒
     * @param post_ms: 向后取多少毫秒
     * @return: true=写出了至少一个关键帧开头的完整窗口
     *
     * @note 起点会向后对齐到窗口内第一个关键帧，所以实际时长可能比
     *       pre_ms+post_ms 短；GOP 越小这个缩水越小（默认 1s）。
     */
    bool dumpAnnexB(const QString &filePath, qint64 center_ms, qint64 pre_ms, qint64 post_ms);

    /**
     * @brief dump 同一段窗口为 MP4（网页/播放器直接可播的副本）
     * @param filePath: 输出路径（.mp4）
     * @note 与 dumpAnnexB 同一批包、同一次拷贝，只多一层 remux 封装，
     *       不重新编码（浏览器放不了 Annex-B 裸流）。
     *       分流参数取 init() 的快照，因此可以和解码线程的 encodeFrame 并发调用。
     */
    bool dumpMp4(const QString &filePath, qint64 center_ms, qint64 pre_ms, qint64 post_ms);

    bool isInitialized() const { return initialized_; }

    int bufferedFrameCount() const;
    int bufferSeconds() const { return cfg_.bufferSeconds; }

    /** @brief 累计因池耗尽/编码失败丢掉的帧数（用于判断负载是否吃紧） */
    qint64 droppedFrameCount() const { return droppedFrames_.load(std::memory_order_relaxed); }
    /** @brief 累计成功编码的帧数 */
    qint64 encodedFrameCount() const { return encodedFrames_.load(std::memory_order_relaxed); }
    /** @brief 实际使用的编码器名（init 后有效），便于日志与网页展示 */
    QString encoderName() const { return encoderName_; }

private:
    struct BufferedPacket
    {
        std::vector<uint8_t> data;  ///< Annex-B 码流（含起始码，关键帧自带参数集）
        qint64 pts = 0;             ///< 编码器 time_base 下的 tick
        qint64 wall_ms = 0;         ///< 本机毫秒墙钟，窗口选取用
        bool isKeyFrame = false;
    };

    bool openEncoder(const char *name);
    /// 取 [center-pre, center+post] 窗口并把包拷出（锁内只做拷贝）
    bool collectWindow(qint64 center_ms, qint64 pre_ms, qint64 post_ms,
                       std::vector<BufferedPacket> &out) const;
    /// 收走编码器已产出的包并入环（EAGAIN 即返回，不等下一帧）
    void collectPackets();
    void pushPacket(AVPacket *pkt);
    void releaseEncoder();

    RecorderConfig cfg_;

    // ===== 硬件编码资源 =====
    AVBufferRef *hwDeviceCtx_ = nullptr;   ///< rkmpp 设备
    AVBufferRef *hwFramesCtx_ = nullptr;   ///< DRM_PRIME/NV12 帧池（RGA 直写）
    AVCodecContext *codecCtx_ = nullptr;   ///< hevc_rkmpp / h264_rkmpp
    AVFrame *poolFrame_ = nullptr;         ///< 复用的池帧句柄（每帧 get_buffer）
    /// init() 时从 codecCtx_ 拷贝的分流参数，供后台 dump 线程 remux MP4 用：
    /// 编码器上下文不是线程安全的，读它必须走这份快照而不是 codecCtx_ 本身。
    AVCodecParameters *streamParams_ = nullptr;
    QString encoderName_;
    bool initialized_ = false;
    int width_ = 0;
    int height_ = 0;

    // ===== 时间戳基准 =====
    qint64 firstWallMs_ = 0;   ///< 首帧墙钟，供窗口/落库参考
    // 流时间线用单调帧计数折算（steady_pts = encodeIndex_ * 1000 / fps），
    // 与到达墙钟解耦：解码/网络抖动不再进 HLS/fMP4 时间线。编码器 time_base=1/fps，
    // 所以 AVFrame->pts 直接取 encodeIndex_。报警切片窗口另用 wall_ms（见下）。
    qint64 encodeIndex_ = 0;
    // 最近一次送入编码器的帧时间戳。编码器输出必然属于它之前的帧，
    // 而"池帧借不到先排空编码器"这条路径发生在本帧 send_frame 之前，
    // 所以收包时要用的正是上一帧的时间戳，不能取当前帧的。
    qint64 lastPts_ = 0;
    qint64 lastWallMs_ = 0;

    // ===== 环形缓冲区 =====
    mutable QMutex mutex_;
    std::vector<BufferedPacket> ringBuffer_;
    int maxPackets_ = 0;

    std::atomic<qint64> encodedFrames_{0};
    std::atomic<qint64> droppedFrames_{0};
};

#endif // VIDEO_RECORDER_H
