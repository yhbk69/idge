// ============================================================================
// VideoRecorder - 视频录制器实现
// ============================================================================
//
// 实现环形缓冲区视频录制功能：
//   1. RGBA 帧 → YUV420 转换（使用 libswscale）
//   2. YUV420 帧 → H.264 编码（使用 FFmpeg 软件编码器）
//   3. H.264 数据包 → 环形缓冲区（内存存储）
//   4. 报警触发 → 缓冲区 dump 到 MP4 文件
//
// ============================================================================

#include "video_recorder.h"
#include <QDebug>
#include <QDir>
#include <QDateTime>

// ============================================================================
// 构造函数
// ============================================================================
VideoRecorder::VideoRecorder(int bufferSeconds, int fps, QObject *parent)
    : QObject(parent)
    , bufferSeconds_(bufferSeconds)
    , fps_(fps)
{
    // 计算缓冲区最大帧数
    maxFrames_ = bufferSeconds_ * fps_;
    ringBuffer_.reserve(maxFrames_);
}

// ============================================================================
// 析构函数
// ============================================================================
VideoRecorder::~VideoRecorder()
{
    // 释放编码器资源
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_);
    }
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
    }
    if (yuvFrame_) {
        av_frame_free(&yuvFrame_);
    }
}

// ============================================================================
// init: 初始化编码器
// ============================================================================
// 流程：
//   1. 查找 H.264 软件编码器
//   2. 配置编码参数（分辨率、帧率、码率）
//   3. 分配 YUV420 帧缓冲
//   4. 初始化 RGBA→YUV420 转换上下文
// ============================================================================
bool VideoRecorder::init(int width, int height)
{
    if (initialized_) {
        return true;
    }

    width_ = width;
    height_ = height;

    // 1. 查找 H.264 编码器
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        qWarning() << "VideoRecorder: H.264 encoder not found";
        return false;
    }

    // 2. 分配编码器上下文
    codecCtx_ = avcodec_alloc_context3(codec);
    if (!codecCtx_) {
        qWarning() << "VideoRecorder: Failed to allocate codec context";
        return false;
    }

    // 3. 配置编码参数
    codecCtx_->codec_id = AV_CODEC_ID_H264;
    codecCtx_->codec_type = AVMEDIA_TYPE_VIDEO;
    codecCtx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codecCtx_->width = width;
    codecCtx_->height = height;
    codecCtx_->bit_rate = 2000000;  // 2 Mbps（适中质量）
    codecCtx_->time_base = {1, fps_};
    codecCtx_->framerate = {fps_, 1};
    codecCtx_->gop_size = fps_ * 2;  // 每 2 秒一个关键帧
    codecCtx_->max_b_frames = 0;     // 不使用 B 帧（减少延迟）

    // 使用 ultrafast 预设（最低延迟，适合实时录制）
    av_opt_set(codecCtx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codecCtx_->priv_data, "tune", "zerolatency", 0);

    // 4. 打开编码器
    int ret = avcodec_open2(codecCtx_, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        qWarning() << "VideoRecorder: Failed to open encoder:" << errBuf;
        avcodec_free_context(&codecCtx_);
        return false;
    }

    // 5. 分配 YUV420 帧缓冲
    yuvFrame_ = av_frame_alloc();
    if (!yuvFrame_) {
        qWarning() << "VideoRecorder: Failed to allocate frame";
        avcodec_free_context(&codecCtx_);
        return false;
    }
    yuvFrame_->format = codecCtx_->pix_fmt;
    yuvFrame_->width = width;
    yuvFrame_->height = height;
    ret = av_frame_get_buffer(yuvFrame_, 32);
    if (ret < 0) {
        qWarning() << "VideoRecorder: Failed to allocate frame buffer";
        av_frame_free(&yuvFrame_);
        avcodec_free_context(&codecCtx_);
        return false;
    }

    // 6. 初始化 RGBA→YUV420 转换上下文
    swsCtx_ = sws_getContext(
        width, height, AV_PIX_FMT_RGBA,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!swsCtx_) {
        qWarning() << "VideoRecorder: Failed to initialize SWS context";
        av_frame_free(&yuvFrame_);
        avcodec_free_context(&codecCtx_);
        return false;
    }

    initialized_ = true;
    qInfo() << "VideoRecorder: Initialized" << width << "x" << height
            << "buffer=" << bufferSeconds_ << "s (" << maxFrames_ << " frames)";
    return true;
}

// ============================================================================
// encodeFrame: 编码一帧并存入环形缓冲区
// ============================================================================
// 流程：
//   1. RGBA → YUV420 转换（使用 libswscale）
//   2. YUV420 → H.264 编码
//   3. 编码后的数据包存入环形缓冲区
// ============================================================================
void VideoRecorder::encodeFrame(const unsigned char *rgbaData, int width, int height, qint64 timestamp_ms)
{
    if (!initialized_ || width != width_ || height != height_) {
        return;
    }

    // 1. RGBA → YUV420 转换
    av_frame_make_writable(yuvFrame_);

    const uint8_t *srcSlice[1] = { rgbaData };
    int srcStride[1] = { width * 4 };  // RGBA: 4 字节/像素

    sws_scale(swsCtx_,
              srcSlice, srcStride,
              0, height,
              yuvFrame_->data, yuvFrame_->linesize);

    yuvFrame_->pts = timestamp_ms;  // 使用毫秒时间戳

    // 2. 编码
    encodePacket(rgbaData, width, height, timestamp_ms);
}

// ============================================================================
// encodePacket: 编码一帧为 H.264 并存入缓冲区
// ============================================================================
bool VideoRecorder::encodePacket(const unsigned char *rgbaData, int width, int height, qint64 pts)
{
    // 发送帧到编码器
    int ret = avcodec_send_frame(codecCtx_, yuvFrame_);
    if (ret < 0) {
        return false;
    }

    // 接收编码后的包
    AVPacket *pkt = av_packet_alloc();
    while (ret >= 0) {
        ret = avcodec_receive_packet(codecCtx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            av_packet_free(&pkt);
            return false;
        }

        // 存入环形缓冲区
        bufferPacket(pkt, pts);
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return true;
}

// ============================================================================
// bufferPacket: 将编码后的包存入环形缓冲区
// ============================================================================
void VideoRecorder::bufferPacket(AVPacket *pkt, qint64 pts)
{
    QMutexLocker lock(&mutex_);

    // 如果缓冲区满，删除最旧的帧
    if ((int)ringBuffer_.size() >= maxFrames_) {
        ringBuffer_.erase(ringBuffer_.begin());
    }

    // 存入新帧
    BufferedPacket bp;
    bp.data.assign(pkt->data, pkt->data + pkt->size);
    bp.pts = pts;
    bp.isKeyFrame = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    ringBuffer_.push_back(std::move(bp));
}

// ============================================================================
// dumpToFile: 将环形缓冲区内容写入 MP4 文件
// ============================================================================
// 流程：
//   1. 创建 MP4 输出上下文
//   2. 复制编码器参数到输出流
//   3. 按时间顺序写入所有缓冲的 H.264 数据包
//   4. 尾部写入 flush packet
// ============================================================================
bool VideoRecorder::dumpToFile(const QString &filePath)
{
    QMutexLocker lock(&mutex_);

    if (ringBuffer_.empty()) {
        qWarning() << "VideoRecorder: Buffer is empty, nothing to dump";
        return false;
    }

    // 确保输出目录存在
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 1. 创建输出格式上下文
    AVFormatContext *fmtCtx = nullptr;
    int ret = avformat_alloc_output_context2(&fmtCtx, nullptr, nullptr, filePath.toUtf8().constData());
    if (ret < 0 || !fmtCtx) {
        qWarning() << "VideoRecorder: Failed to create output context";
        return false;
    }

    // 2. 创建输出流
    AVStream *stream = avformat_new_stream(fmtCtx, nullptr);
    if (!stream) {
        avformat_free_context(fmtCtx);
        return false;
    }

    // 3. 复制编码器参数
    ret = avcodec_parameters_from_context(stream->codecpar, codecCtx_);
    if (ret < 0) {
        avformat_free_context(fmtCtx);
        return false;
    }
    stream->time_base = codecCtx_->time_base;

    // 4. 打开输出文件
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmtCtx->pb, filePath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            qWarning() << "VideoRecorder: Failed to open output file:" << filePath;
            avformat_free_context(fmtCtx);
            return false;
        }
    }

    // 5. 写入文件头
    ret = avformat_write_header(fmtCtx, nullptr);
    if (ret < 0) {
        qWarning() << "VideoRecorder: Failed to write header";
        avio_closep(&fmtCtx->pb);
        avformat_free_context(fmtCtx);
        return false;
    }

    // 6. 写入所有缓冲的数据包
    qint64 basePts = ringBuffer_.front().pts;
    int writtenFrames = 0;

    for (const auto &bp : ringBuffer_) {
        AVPacket *pkt = av_packet_alloc();
        pkt->data = const_cast<uint8_t*>(bp.data.data());
        pkt->size = (int)bp.data.size();
        pkt->pts = bp.pts - basePts;  // 相对时间戳
        pkt->dts = pkt->pts;
        pkt->stream_index = 0;
        if (bp.isKeyFrame) {
            pkt->flags |= AV_PKT_FLAG_KEY;
        }

        ret = av_interleaved_write_frame(fmtCtx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            qWarning() << "VideoRecorder: Failed to write packet";
            break;
        }
        writtenFrames++;
    }

    // 7. 写入文件尾
    av_write_trailer(fmtCtx);

    // 8. 清理
    if (!(fmtCtx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&fmtCtx->pb);
    }
    avformat_free_context(fmtCtx);

    qInfo() << "VideoRecorder: Dumped" << writtenFrames << "frames to" << filePath;
    return true;
}

// ============================================================================
// bufferedFrameCount: 获取缓冲区中的帧数
// ============================================================================
int VideoRecorder::bufferedFrameCount() const
{
    QMutexLocker lock(&mutex_);
    return ringBuffer_.size();
}
