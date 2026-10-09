// ============================================================================
// VideoRecorder - 硬件编码环形缓冲区录制器实现
// ============================================================================
//
// 见 video_recorder.h 顶部的链路与实测结论说明。
// 本文件的三个关键约束：
//   1. 编码全程零拷贝：RGA 在 fd 之间搬像素，CPU 只做参数构造与调用；
//   2. 绝不阻塞解码线程：池帧借不到就先收包再试一次，仍失败即丢帧计数；
//   3. dump 锁内只拷数据，文件 IO 放锁外。
//
// ============================================================================

#include "video_recorder.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "rga_converter.h"

// 帧池大小：编码器在途帧(实测 1~2) + RGA 正在写的 1 帧，8 有余量
// （2026-09-29 板端 enc_probe：515 帧零池枯竭）
static constexpr int kHwFramePoolSize = 8;

// ============================================================================
// 构造 / 析构
// ============================================================================
VideoRecorder::VideoRecorder(const RecorderConfig &cfg)
    : cfg_(cfg)
{
    if (cfg_.fps <= 0) {
        cfg_.fps = 25;
    }
    if (cfg_.bufferSeconds <= 0) {
        cfg_.bufferSeconds = 5;
    }
    if (cfg_.gopSeconds <= 0) {
        cfg_.gopSeconds = 1;
    }
    maxPackets_ = cfg_.bufferSeconds * cfg_.fps;
    ringBuffer_.reserve(maxPackets_);
}

VideoRecorder::~VideoRecorder()
{
    releaseEncoder();
}

void VideoRecorder::releaseEncoder()
{
    if (poolFrame_) {
        av_frame_free(&poolFrame_);
    }
    if (streamParams_) {
        avcodec_parameters_free(&streamParams_);
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    if (hwFramesCtx_) {
        av_buffer_unref(&hwFramesCtx_);
    }
    if (hwDeviceCtx_) {
        av_buffer_unref(&hwDeviceCtx_);
    }
    initialized_ = false;
}

// ============================================================================
// openEncoder: 建硬编码器上下文（需 hwDeviceCtx_/hwFramesCtx_ 已就绪）
// ============================================================================
bool VideoRecorder::openEncoder(const char *name)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
        qWarning() << "VideoRecorder: encoder not found:" << name;
        return false;
    }

    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        qWarning() << "VideoRecorder: alloc codec context failed";
        return false;
    }

    codecCtx_->hw_device_ctx = av_buffer_ref(hwDeviceCtx_);
    codecCtx_->hw_frames_ctx = av_buffer_ref(hwFramesCtx_);
    // 输入是 DRM_PRIME 引用，真实像素布局由帧池 sw_format(NV12) 决定
    codecCtx_->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    codecCtx_->width = width_;
    codecCtx_->height = height_;
    codecCtx_->time_base = {1, cfg_.fps};
    codecCtx_->framerate = {cfg_.fps, 1};
    codecCtx_->bit_rate = cfg_.bitRate;
    // GOP 决定"报警窗口前向对齐关键帧"的误差上限，默认 1 秒一个 IDR
    codecCtx_->gop_size = cfg_.fps * cfg_.gopSeconds;
    codecCtx_->max_b_frames = 0;   // 裸流按发送顺序 dump，B 帧会打乱解码顺序

    // rc_mode=1 → MPP CBR：上传带宽可预测
    if (av_opt_set_int(codecCtx_->priv_data, "rc_mode", 1, 0) < 0) {
        qWarning() << "VideoRecorder:" << name << "无 rc_mode 选项，沿用编码器默认码控";
    }

    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        qWarning() << "VideoRecorder: open encoder" << name << "failed:" << errBuf;
        avcodec_free_context(&codecCtx_);
        return false;
    }

    encoderName_ = QString::fromUtf8(name);
    return true;
}

// ============================================================================
// init: rkmpp 设备 → DRM_PRIME/NV12 帧池 → 硬编码器
// ============================================================================
bool VideoRecorder::init(int width, int height)
{
    if (initialized_) {
        return true;
    }
    if (width <= 0 || height <= 0) {
        qWarning() << "VideoRecorder: invalid size" << width << "x" << height;
        return false;
    }

    width_ = width;
    height_ = height;

    const enum AVHWDeviceType devType = av_hwdevice_find_type_by_name("rkmpp");
    if (devType == AV_HWDEVICE_TYPE_NONE) {
        qWarning() << "VideoRecorder: 本 FFmpeg 无 rkmpp 设备，录制关闭（不回落 CPU 软编）";
        return false;
    }
    int ret = av_hwdevice_ctx_create(&hwDeviceCtx_, devType, nullptr, nullptr, 0);
    if (ret < 0) {
        qWarning() << "VideoRecorder: create rkmpp device failed, ret =" << ret;
        releaseEncoder();
        return false;
    }

    // 帧池：RGA 把画好框的 RGBA 直接转成 NV12 写进这些 DMA-BUF，
    // 编码器从同一个 fd 读，全程没有 CPU 参与像素搬运。
    hwFramesCtx_ = av_hwframe_ctx_alloc(hwDeviceCtx_);
    if (!hwFramesCtx_) {
        qWarning() << "VideoRecorder: alloc hwframe ctx failed";
        releaseEncoder();
        return false;
    }
    AVHWFramesContext *fc = reinterpret_cast<AVHWFramesContext *>(hwFramesCtx_->data);
    fc->format = AV_PIX_FMT_DRM_PRIME;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = width_;
    fc->height = height_;
    fc->initial_pool_size = kHwFramePoolSize;
    ret = av_hwframe_ctx_init(hwFramesCtx_);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        qWarning() << "VideoRecorder: init hwframe pool failed:" << errBuf;
        releaseEncoder();
        return false;
    }

    // D2=b：主用 H.265（带宽减半），退档只到 H.264 硬编——两者都是硬件路径，
    // 不存在"退到 CPU 拖垮主流程"的中间态。
    const bool wantH264 = (cfg_.codec.compare("h264", Qt::CaseInsensitive) == 0);
    const char *primary = wantH264 ? "h264_rkmpp" : "hevc_rkmpp";
    const char *secondary = wantH264 ? "hevc_rkmpp" : "h264_rkmpp";

    if (!openEncoder(primary) && !openEncoder(secondary)) {
        qWarning() << "VideoRecorder: rkmpp 编码器均不可用，录制关闭";
        releaseEncoder();
        return false;
    }

    poolFrame_ = av_frame_alloc();
    if (!poolFrame_) {
        qWarning() << "VideoRecorder: alloc pool frame failed";
        releaseEncoder();
        return false;
    }

    // 快照分流参数：dumpMp4 由报警后台线程调用，与解码线程的 send/receive 并发，
    // 绝不能在 dump 里去读 codecCtx_（编码器上下文不是线程安全的）。
    // 打开后 extradata_size=0（参数集内嵌在每个 IDR 前），MP4 muxer 的自动 bsf
    // 会从裸流里抽出 hvcC/avcC，所以这份 init 时刻的快照与实时读取等价。
    streamParams_ = avcodec_parameters_alloc();
    if (!streamParams_) {
        qWarning() << "VideoRecorder: alloc codec params failed";
        releaseEncoder();
        return false;
    }
    ret = avcodec_parameters_from_context(streamParams_, codecCtx_);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        qWarning() << "VideoRecorder: snapshot codec params failed:" << errBuf;
        releaseEncoder();
        return false;
    }

    firstWallMs_ = 0;
    encodeIndex_ = 0;
    lastPts_ = 0;
    lastWallMs_ = 0;
    initialized_ = true;
    qInfo() << "VideoRecorder:" << encoderName_ << "ready" << width_ << "x" << height_
            << "buffer=" << cfg_.bufferSeconds << "s (" << maxPackets_ << "pkts)"
            << "gop=" << codecCtx_->gop_size << "bitrate=" << cfg_.bitRate;
    return true;
}

// ============================================================================
// collectPackets: 收走编码器已产出的包并入环
// ============================================================================
void VideoRecorder::collectPackets()
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return;
    }
    while (avcodec_receive_packet(codecCtx_, pkt) == 0) {
        pushPacket(pkt);
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
}

// ============================================================================
// pushPacket: 存入环形缓冲区（满则丢最旧）
// ============================================================================
void VideoRecorder::pushPacket(AVPacket *pkt)
{
    QMutexLocker lock(&mutex_);

    if (static_cast<int>(ringBuffer_.size()) >= maxPackets_) {
        ringBuffer_.erase(ringBuffer_.begin());
    }

    BufferedPacket bp;
    // 同一帧可能产出多个包（参数集 + slice），它们共用该帧的时间戳，
    // 时间戳取"最近一次成功送入编码器的帧"，因为收到的包必属于它之前的帧。
    bp.data.assign(pkt->data, pkt->data + pkt->size);
    bp.pts = lastPts_;
    bp.wall_ms = lastWallMs_;
    bp.isKeyFrame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    ringBuffer_.push_back(std::move(bp));
}

// ============================================================================
// encodeFrame: RGBA DMA-BUF → RGA → NV12 池帧 → 硬编 → ring
// ============================================================================
void VideoRecorder::encodeFrame(int rgba_fd, int w, int h, int rgba_wstride_px, qint64 wall_ms)
{
    if (!initialized_ || w != width_ || h != height_) {
        return;
    }
    if (rgba_fd < 0 || rgba_wstride_px < w || rgba_wstride_px > w * 2) {
        // stride 口径错误：把字节 pitch 当像素跨度传了（RGBA 会正好大 4 倍），
        // 或对齐 padding 超出合理范围。此时 RGA 会按 4 倍尺寸去 map 一块
        // 只有 1/4 大小的 DMA-BUF，内核只留一条无法定位的 map 失败日志，
        // 所以在进硬件之前就拦下并计数。
        qWarning() << "VideoRecorder: bad RGBA stride" << rgba_wstride_px << "for width" << w;
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (firstWallMs_ == 0) {
        firstWallMs_ = wall_ms;
    }
    // 流时间线用单调帧计数（steady_pts），与到达墙钟解耦：
    // 解码/网络抖动不再进 pts，HLS/fMP4 切片节拍严格等距。wall_ms 仅入 bp 供窗口选取。
    const qint64 pts = encodeIndex_;

    av_frame_unref(poolFrame_);
    int ret = av_hwframe_get_buffer(hwFramesCtx_, poolFrame_, 0);
    if (ret < 0) {
        // 编码器还占着池帧：先收包再试一次，仍失败就丢帧（绝不等待）
        collectPackets();
        ret = av_hwframe_get_buffer(hwFramesCtx_, poolFrame_, 0);
        if (ret < 0) {
            droppedFrames_.fetch_add(1, std::memory_order_relaxed);
            av_frame_unref(poolFrame_);
            return;
        }
    }

    const AVDRMFrameDescriptor *desc =
        reinterpret_cast<const AVDRMFrameDescriptor *>(poolFrame_->data[0]);
    if (!desc || desc->nb_objects < 1 || desc->nb_layers < 1) {
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        av_frame_unref(poolFrame_);
        return;
    }
    const int dstFd = desc->objects[0].fd;
    // NV12 的 Y 平面是 1 字节/像素，所以 planes[0].pitch（字节）数值上
    // 就等于 RGA 需要的像素跨度；UV 平面按对齐取整，垂直跨度必须用
    // 两个 plane 的 offset 差反推，不能用 height。
    const int pitch = desc->layers[0].planes[0].pitch;
    if (dstFd < 0 || pitch < w) {
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        av_frame_unref(poolFrame_);
        return;
    }
    const int vstride = (desc->layers[0].nb_planes > 1)
                            ? static_cast<int>((desc->layers[0].planes[1].offset -
                                                 desc->layers[0].planes[0].offset) / pitch)
                            : h;

    // 这步是零拷贝链路最常见的故障点（stride/格式不符），
    // RgaConverter 内部已打印内核侧原因，这里只计数，不每帧刷日志。
    if (!RgaConverter::rgba_to_nv12(rgba_fd, w, h, rgba_wstride_px, dstFd, w, h, pitch, vstride)) {
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
        av_frame_unref(poolFrame_);
        return;
    }

    poolFrame_->pts = pts;
    lastPts_ = pts;
    lastWallMs_ = wall_ms;

    ret = avcodec_send_frame(codecCtx_, poolFrame_);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        qWarning() << "VideoRecorder: send_frame failed:" << errBuf;
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        encodedFrames_.fetch_add(1, std::memory_order_relaxed);
        ++encodeIndex_;   // 成功送入编码器才推进单调帧计数（失败不占用 tick，避免流时间线留洞）
    }

    collectPackets();
    av_frame_unref(poolFrame_);
}

// ============================================================================
// collectWindow: 取 [center-pre, center+post] 窗口，起点向后对齐关键帧
// ============================================================================
bool VideoRecorder::collectWindow(qint64 center_ms, qint64 pre_ms, qint64 post_ms,
                                  std::vector<BufferedPacket> &out) const
{
    out.clear();
    QMutexLocker lock(&mutex_);

    if (ringBuffer_.empty()) {
        return false;
    }

    const qint64 beginMs = center_ms - pre_ms;
    const qint64 endMs = center_ms + post_ms;

    // 起点必须落在关键帧：从半路 slice 开始的裸流/MP4 必然花屏。
    // 在 [beginMs, endMs] 内找第一个关键帧，允许窗口前向缩水（最多一个 GOP）。
    int startIdx = -1;
    for (int i = 0; i < static_cast<int>(ringBuffer_.size()); ++i) {
        if (ringBuffer_[i].wall_ms >= beginMs && ringBuffer_[i].isKeyFrame) {
            startIdx = i;
            break;
        }
    }
    if (startIdx < 0) {
        return false;
    }

    for (int i = startIdx; i < static_cast<int>(ringBuffer_.size()); ++i) {
        const BufferedPacket &bp = ringBuffer_[i];
        if (bp.wall_ms > endMs) {
            break;
        }
        out.push_back(bp);   // 锁内只做拷贝，文件 IO 留给调用方在锁外做
    }

    return !out.empty();
}

// ============================================================================
// dumpAnnexB: 写出 H.265/H.264 Annex-B 裸流（上传服务器用）
// ============================================================================
bool VideoRecorder::dumpAnnexB(const QString &filePath, qint64 center_ms,
                               qint64 pre_ms, qint64 post_ms)
{
    std::vector<BufferedPacket> window;
    if (!collectWindow(center_ms, pre_ms, post_ms, window)) {
        qWarning() << "VideoRecorder: buffer has no keyframe in window for" << filePath;
        return false;
    }

    const QFileInfo fi(filePath);
    if (!fi.absoluteDir().exists() && !QDir().mkpath(fi.absolutePath())) {
        qWarning() << "VideoRecorder: mkdir failed" << fi.absolutePath();
        return false;
    }

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "VideoRecorder: open failed" << filePath << f.errorString();
        return false;
    }

    qint64 bytes = 0;
    for (const BufferedPacket &bp : window) {
        const qint64 written = f.write(reinterpret_cast<const char *>(bp.data.data()),
                                       static_cast<qint64>(bp.data.size()));
        if (written < 0) {
            qWarning() << "VideoRecorder: write failed" << filePath << f.errorString();
            f.close();
            return false;
        }
        bytes += written;
    }
    f.close();

    qInfo() << "VideoRecorder: AnnexB" << filePath
            << static_cast<int>(window.size()) << "pkts" << bytes << "bytes"
            << (window.back().wall_ms - window.front().wall_ms) / 1000.0 << "s";
    return true;
}

// ============================================================================
// dumpMp4: 同一批包 remux 成 MP4（网页/播放器副本，不重新编码）
// ============================================================================
// 编码器把 VPS/SPS/PPS 内嵌在每个 IDR 之前，MP4 muxer 会自动抽出成
// hvcC/avcC（板端实测：hevc_rkmpp 裸流 → MP4 后 extradata_size=109）。
// ============================================================================
bool VideoRecorder::dumpMp4(const QString &filePath, qint64 center_ms,
                            qint64 pre_ms, qint64 post_ms)
{
    std::vector<BufferedPacket> window;
    if (!collectWindow(center_ms, pre_ms, post_ms, window)) {
        qWarning() << "VideoRecorder: buffer has no keyframe in window for" << filePath;
        return false;
    }

    const QFileInfo fi(filePath);
    if (!fi.absoluteDir().exists() && !QDir().mkpath(fi.absolutePath())) {
        qWarning() << "VideoRecorder: mkdir failed" << fi.absolutePath();
        return false;
    }

    AVFormatContext *fmtCtx = nullptr;
    int ret = avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr,
                                             filePath.toUtf8().constData());
    if (ret < 0 || !fmtCtx) {
        qWarning() << "VideoRecorder: alloc output context failed" << filePath;
        return false;
    }

    AVStream *stream = avformat_new_stream(fmtCtx, nullptr);
    if (!stream) {
        qWarning() << "VideoRecorder: new stream failed";
        avformat_free_context(fmtCtx);
        return false;
    }

    bool ok = false;
    AVPacket *pkt = av_packet_alloc();
    do {
        // 只用 init() 时的参数快照，不碰 codecCtx_（见上方线程模型说明）
        ret = avcodec_parameters_copy(stream->codecpar, streamParams_);
        if (ret < 0) {
            qWarning() << "VideoRecorder: copy codec params failed";
            break;
        }
        stream->time_base = {1, cfg_.fps};

        if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&fmtCtx->pb, filePath.toUtf8().constData(), AVIO_FLAG_WRITE);
            if (ret < 0) {
                qWarning() << "VideoRecorder: avio_open failed" << filePath;
                break;
            }
        }

        ret = avformat_write_header(fmtCtx, nullptr);
        if (ret < 0) {
            char errBuf[128];
            av_strerror(ret, errBuf, sizeof(errBuf));
            qWarning() << "VideoRecorder: write header failed:" << errBuf;
            break;
        }

        // 包数据是借用的（pkt->buf = NULL），write_frame 只会 unref 不会 free，
        // 因此不能拷贝、也绝不能让 ffmpeg 去 free 这块 std::vector 内存。
        // 输出时间戳按墙钟差折算，同一帧的多个包用 +i 保证严格递增（MP4 要求
        // dts 单调），比直接复用 bp.pts 更抗丢帧。
        const qint64 baseWallMs = window.front().wall_ms;
        qint64 lastOutPts = -1;
        int index = 0;
        for (const BufferedPacket &bp : window) {
            qint64 outPts = (bp.wall_ms - baseWallMs) * cfg_.fps / 1000;
            if (outPts <= lastOutPts) {
                outPts = lastOutPts + 1;
            }
            lastOutPts = outPts;

            pkt->data = const_cast<uint8_t *>(bp.data.data());
            pkt->size = static_cast<int>(bp.data.size());
            pkt->pts = pkt->dts = outPts;
            pkt->duration = 1;
            pkt->stream_index = 0;
            pkt->flags = bp.isKeyFrame ? AV_PKT_FLAG_KEY : 0;

            ret = av_interleaved_write_frame(fmtCtx, pkt);
            av_packet_unref(pkt);
            ++index;
            if (ret < 0) {
                qWarning() << "VideoRecorder: write packet failed at" << index << "ret =" << ret;
                break;
            }
        }

        av_write_trailer(fmtCtx);
        ok = true;
    } while (false);

    if (pkt) {
        av_packet_free(&pkt);
    }
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmtCtx->pb);
    }
    avformat_free_context(fmtCtx);

    if (ok) {
        qInfo() << "VideoRecorder: MP4" << filePath
                << static_cast<int>(window.size()) << "pkts"
                << (window.back().wall_ms - window.front().wall_ms) / 1000.0 << "s";
    }
    return ok;
}

// ============================================================================
// bufferedFrameCount
// ============================================================================
int VideoRecorder::bufferedFrameCount() const
{
    QMutexLocker lock(&mutex_);
    return static_cast<int>(ringBuffer_.size());
}
