// ============================================================================
// video_decoder.cpp - FFmpeg 软件视频解码器
// ============================================================================
//
// 功能：
//   使用 FFmpeg（CPU 软解码）打开本地视频文件或网络流，
//   逐帧解码并发送 QImage 给 UI 线程渲染。
//
// 与 hardware_decode（ffmpeg_video_decoder.cpp）的区别：
//   - 本文件：FFmpeg CPU 软解码，输出 QImage（CPU 内存），适用于播放器控件
//   - ffmpeg_video_decoder.cpp：MPP 硬件解码，输出 DMA-BUF fd（零拷贝），适用于检测流水线
//
// 解码流程：
//   avformat_open_input → avformat_find_stream_info → avcodec_find_decoder
//   → avcodec_open2 → 循环 av_read_frame → avcodec_send_packet
//   → avcodec_receive_frame → sws_scale (YUV→RGB) → emit frameDecoded(QImage)
//
// 帧同步机制：
//   - 基于 PTS（Presentation Time Stamp）的音视频同步
//   - 使用滑动窗口平滑显示耗时，避免卡顿
//   - 支持暂停/恢复（通过互斥锁 + 定时器检查）
//
// ============================================================================

#include "video_decoder.h"
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QtGlobal>

// ============================================================================
// FFmpeg C 头文件（C++ 中必须用 extern "C" 包裹）
// ============================================================================
extern "C"
{
    #include <libavcodec/avcodec.h>      // 编解码器 API
    #include <libavformat/avformat.h>    // 格式容器 API（MP4/RTSP 等）
    #include <libswscale/swscale.h>      // 图像缩放和格式转换（YUV→RGB）
    #include <libavdevice/avdevice.h>    // 设备采集 API（V4L2/X11 等）
    #include <libavformat/version.h>     // FFmpeg 版本宏
    #include <libavutil/time.h>          // 时间工具函数
    #include <libavutil/mathematics.h>   // 数学工具函数（av_rescale_q 等）
    #include <libavutil/imgutils.h>      // 图像内存工具（av_image_fill_arrays 等）
}

// ============================================================================
// 构造函数：初始化 FFmpeg 组件和定时器
// ============================================================================
// 分配 AVPacket（压缩数据包）、AVFrame（解码帧）、RGB帧（转换后）
// 创建暂停检查定时器，每 100ms 检查一次暂停状态
// ============================================================================
VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent)
{
    m_packet = av_packet_alloc();      // 压缩数据包（H.264/H.265 NALU）
    m_frame = av_frame_alloc();        // 解码后的原始帧（YUV/NV12 等）
    m_rgbFrame = av_frame_alloc();     // 转换后的 RGB 帧（用于 QImage）

    m_pauseTimer = new QTimer(this);
    m_pauseTimer->setInterval(100);    // 每 100ms 检查一次暂停状态
    connect(m_pauseTimer, &QTimer::timeout, this, &VideoDecoder::checkPauseState);

    // 使用 QueuedConnection 确保跨线程安全
    connect(this, &VideoDecoder::resumeDecodingNeeded, this, [this]() {
        qDebug() << "Resuming decoding process";
    }, Qt::QueuedConnection);
}

// ============================================================================
// 析构函数：确保解码停止并释放所有资源
// ============================================================================
VideoDecoder::~VideoDecoder()
{
    receiveStopDecoding();
    freeFFmpegResources();

    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_rgbFrame) {
        av_frame_free(&m_rgbFrame);
        m_rgbFrame = nullptr;
    }
}

// ============================================================================
// receiveVideoPath - 打开视频文件并获取基本信息
// ============================================================================
// 步骤：
//   1. 分配格式上下文（AVFormatContext）
//   2. avformat_open_input: 打开视频文件/URL
//   3. avformat_find_stream_info: 获取流信息（编解码参数、帧率等）
//   4. 遍历所有流，找到视频流的索引
//   5. 计算视频总时长（毫秒），通过 sendAllProgress 发送到 UI
//   6. 记录开始时间戳（用于音视频同步计算）
//
// 注意：此函数只打开文件并读取元数据，不开始解码
// ============================================================================
void VideoDecoder::receiveVideoPath(QString path)
{
    m_videoPath = path;

    freeFFmpegResources();  // 先释放旧资源（防止重复初始化）

    // 1. 分配格式上下文（AVFormatContext 管理容器信息）
    m_formatCtx = avformat_alloc_context();

    // 2. 打开视频文件并获取格式信息
    if (avformat_open_input(&m_formatCtx, m_videoPath.toUtf8().constData(), nullptr, nullptr) != 0) {
        qCritical() << "无法打开视频文件:" << m_videoPath;
        emit decodingFinished();
        return;
    }

    // 3. 获取流信息（读取部分帧来分析编码参数）
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qCritical() << "无法获取视频流信息";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    // 4. 查找视频流索引（一个文件可能包含音频、视频、字幕等多种流）
    m_videoStreamIndex = -1;
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; ++i) {
        if (m_formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            break;
        }
    }
    if (m_videoStreamIndex == -1) {
        qCritical() << "未找到视频流";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    // 5. 获取视频时长（AV_TIME_BASE = 1000000，即微秒）
    m_totalDurationMs = m_formatCtx->duration;
    if (m_totalDurationMs == AV_NOPTS_VALUE) {
        qDebug() << "无法获取视频时长";
        return;
    }
    m_totalDurationMs = m_totalDurationMs * 1000 / AV_TIME_BASE;  // 转换为毫秒
    qDebug() << "视频时长ms:" << m_totalDurationMs;
    emit sendAllProgress(m_totalDurationMs);

    // 6. 记录开始时间戳（用于音视频同步）
    m_startRealTime = QDateTime::currentMSecsSinceEpoch();
}

// ============================================================================
// receiveStartDecoding - 开始解码主循环
// ============================================================================
// 步骤：
//   1. 初始化解码器（AVCodec → AVCodecContext）
//   2. 获取实际帧率（avg_frame_rate 或 r_frame_rate）
//   3. 初始化 RGB 转换缓冲区（av_image_fill_arrays）
//   4. 进入解码主循环：
//      a. 检查暂停状态（如有暂停则循环等待）
//      b. av_read_frame: 读取一帧压缩数据
//      c. avcodec_send_packet: 发送到解码器
//      d. avcodec_receive_frame: 获取解码帧（YUV/NV12）
//      e. sws_scale: YUV→RGB32 转换
//      f. 构造 QImage 并通过 signal 发送到 UI 线程
//      g. 基于 PTS 的帧同步（控制播放速度）
//   5. 解码结束，释放资源，发送 decodingFinished 信号
//
// 线程安全：
//   - m_stateMutex: 保护 m_isDecoding 标志
//   - m_pauseMutex: 保护 m_isPaused 标志
//   - m_frameTimer: 帧间隔计时（同步用）
// ============================================================================
void VideoDecoder::receiveStartDecoding()
{
    m_count = 0;

    qint64 m_startTime = QDateTime::currentMSecsSinceEpoch();

    {
        QMutexLocker locker(&m_stateMutex);
        m_isDecoding = true;
    }

    // 4. 初始化解码器
    AVCodecParameters *codecPar = m_formatCtx->streams[m_videoStreamIndex]->codecpar;
    m_codec = avcodec_find_decoder(codecPar->codec_id);
    if (!m_codec) {
        qCritical() << "未找到对应的解码器";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    m_codecCtx = avcodec_alloc_context3(m_codec);
    if (avcodec_parameters_to_context(m_codecCtx, codecPar) < 0) {
        qCritical() << "无法将流参数转换为解码器上下文";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    if (avcodec_open2(m_codecCtx, m_codec, nullptr) < 0) {
        qCritical() << "无法打开解码器";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    // 获取实际帧率（用于无 PTS 时的固定帧率播放）
    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIndex];
    double fps = av_q2d(videoStream->avg_frame_rate);
    if (fps <= 0) {
        fps = av_q2d(videoStream->r_frame_rate);
    }
    m_fps = fps;
    qDebug() << "m_fps:" << m_fps;

    // 发送视频原始信息到主线程（用于 UI 初始化画布大小）
    emit videoInfoDecoded(m_codecCtx->width, m_codecCtx->height);

    // 6. 初始化 RGB 转换资源
    int rgbBufferSize = av_image_get_buffer_size(
        AV_PIX_FMT_RGB32,
        m_codecCtx->width,
        m_codecCtx->height,
        1
    );

    m_rgbBuffer = (unsigned char*)av_malloc(rgbBufferSize);
    av_image_fill_arrays(
        m_rgbFrame->data,
        m_rgbFrame->linesize,
        m_rgbBuffer,
        AV_PIX_FMT_RGB32,
        m_codecCtx->width,
        m_codecCtx->height,
        1
    );

    // ============================================================================
    // 7. 解码主循环
    // ============================================================================
    while (true) {
        m_frameTimer.start();

        // 处理 Qt 事件队列（确保信号能被接收）
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

        // 检查是否需要停止解码（线程安全）
        {
            QMutexLocker locker(&m_stateMutex);
            if (!m_isDecoding) break;
        }

        // 暂停处理：进入暂停循环，直到用户点击"播放"
        bool shouldPause = false;
        {
            QMutexLocker pauseLocker(&m_pauseMutex);
            shouldPause = m_isPaused;
        }
        if (shouldPause) {
            qDebug() << "Decoder paused, waiting for resume...";
            while (true) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                bool canResume = false;
                {
                    QMutexLocker locker(&m_pauseMutex);
                    canResume = !m_isPaused;
                }
                if (canResume) {
                    qDebug() << "Resuming decoding...";
                    break;
                }
                QThread::msleep(50);
            }
        }

        // 读取一帧压缩数据
        int ret = av_read_frame(m_formatCtx, m_packet);
        if (ret < 0) {
            break;  // 读取完毕（正常结束或文件结尾）
        }

        // 只处理视频流（跳过音频流）
        if (m_packet->stream_index == m_videoStreamIndex) {
            if (avcodec_send_packet(m_codecCtx, m_packet) < 0) {
                qWarning() << "发送数据包到解码器失败";
                av_packet_unref(m_packet);
                continue;
            }

            // 循环接收解码后的帧（一个 packet 可能包含多帧）
            while (true) {
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    qWarning() << "接收解码帧失败";
                    break;
                }

                // 帧同步：获取当前帧的理论播放时间
                getCurrentTime();
                m_frameStartTime = QDateTime::currentMSecsSinceEpoch();

                // YUV → RGB32 格式转换（使用 sws_scale，CPU 实现）
                if (m_swsCtx == nullptr) {
                    m_swsCtx = sws_getContext(
                        m_codecCtx->width,
                        m_codecCtx->height,
                        m_codecCtx->pix_fmt,
                        m_codecCtx->width,
                        m_codecCtx->height,
                        AV_PIX_FMT_RGB32,
                        SWS_BILINEAR,
                        nullptr, nullptr, nullptr
                    );
                }

                if (!m_swsCtx) {
                    qWarning() << "无法创建图像转换上下文";
                    av_frame_unref(m_frame);
                    continue;
                }

                sws_scale(
                    m_swsCtx,
                    (const unsigned char* const*)m_frame->data,
                    m_frame->linesize,
                    0,
                    m_codecCtx->height,
                    m_rgbFrame->data,
                    m_rgbFrame->linesize
                );

                // 构造 QImage 并发送到 UI 线程
                QImage frame(
                    m_rgbFrame->data[0],
                    m_codecCtx->width,
                    m_codecCtx->height,
                    m_rgbFrame->linesize[0],
                    QImage::Format_RGB32
                );

                m_frameSendTime = QDateTime::currentMSecsSinceEpoch();
                emit frameDecoded(frame);

                qDebug() << "send time        " << m_count << ": "
                         << QDateTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch()).toLocalTime().toString("hh:mm:ss.zzz");
                m_count++;

                // 帧同步（基于 PTS + 全局时间偏差校准）
                if (m_frame->pts != AV_NOPTS_VALUE) {
                    if (m_lastFramePts != AV_NOPTS_VALUE) {
                        // 1. 计算单帧理论间隔（两帧 PTS 差 × 时间基）
                        double frameDelay = av_q2d(videoStream->time_base) *
                                (m_frame->pts - m_lastFramePts) * 1000;
                        frameDelay = qBound(0.0, frameDelay, 1000.0);

                        // 2. 减去上一帧的显示耗时（单帧补偿）
                        frameDelay -= (m_lastDisplayCost > 0 ? m_lastDisplayCost : 9);

                        // 3. 全局时间偏差校准（防止长时间播放后音视频不同步）
                        if (m_startRealTime != -1 && m_currentTheoryMs >= 0) {
                            qint64 realElapsed = QDateTime::currentMSecsSinceEpoch() - m_startRealTime;
                            qint64 theoryElapsed = m_currentTheoryMs;
                            int globalDiff = theoryElapsed - realElapsed;

                            // 4. 用全局偏差微调当前帧延迟（每次最多调整 ±5ms，避免突变）
                            frameDelay += qBound(-5, globalDiff / 5, 5);
                        }

                        // 5. 执行休眠
                        int elapsed = m_frameTimer.restart();
                        int remainingDelay = frameDelay - elapsed;
                        remainingDelay = qBound(0, remainingDelay, 100);
                        qDebug() << "remainingDelay:" << remainingDelay;
                        if (remainingDelay > 0) {
                            QThread::msleep(remainingDelay);
                        }
                    }
                    m_lastFramePts = m_frame->pts;
                } else {
                    QThread::msleep(qMax(1, static_cast<int>(1000.0 / m_fps)));
                }
            }
        }

        av_packet_unref(m_packet);
    }

    qint64 totalPlayTime = QDateTime::currentMSecsSinceEpoch() - m_startTime;
    qDebug() << "实际播放耗时(s):" << totalPlayTime;

    av_packet_unref(m_packet);
    freeFFmpegResources();
    emit decodingFinished();
}

// ============================================================================
// receiveStopDecoding - 停止解码（线程安全）
// ============================================================================
void VideoDecoder::receiveStopDecoding()
{
    QMutexLocker locker(&m_stateMutex);
    m_isDecoding = false;
}

// ============================================================================
// freeFFmpegResources - 释放所有 FFmpeg 资源
// ============================================================================
// 释放顺序很重要：先释放转换上下文，再释放缓冲区，最后释放解码器和格式上下文
// ============================================================================
void VideoDecoder::freeFFmpegResources()
{
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);  // 释放缩放上下文
        m_swsCtx = nullptr;
    }
    if (m_rgbBuffer) {
        av_free(m_rgbBuffer);       // 释放 RGB 缓冲区
        m_rgbBuffer = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);  // 释放解码器上下文
        m_codecCtx = nullptr;
    }
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);  // 关闭输入
        avformat_free_context(m_formatCtx);   // 释放格式上下文
        m_formatCtx = nullptr;
    }
    m_codec = nullptr;
}

// ============================================================================
// receivePauseDecoding - 暂停解码（线程安全）
// ============================================================================
void VideoDecoder::receivePauseDecoding(){
    qDebug() << "receivePauseDecoding";
    QMutexLocker locker(&m_pauseMutex);
    if (!m_isPaused) {
        m_isPaused = true;
        qDebug() << "Pause signal received, decoder paused";
    }
}

// ============================================================================
// receiveResumeDecoding - 恢复解码（线程安全）
// ============================================================================
void VideoDecoder::receiveResumeDecoding(){
    qDebug() << "receiveResumeDecoding";
    QMutexLocker locker(&m_pauseMutex);
    if (m_isPaused) {
        m_isPaused = false;
        qDebug() << "Resume signal received, decoder will resume";
    }
}

// ============================================================================
// checkPauseState - 定时器回调，每 100ms 检查暂停状态
// ============================================================================
void VideoDecoder::checkPauseState() {
    QMutexLocker locker(&m_pauseMutex);
    if (!m_isPaused && m_pauseTimer->isActive()) {
        m_pauseTimer->stop();
        emit resumeDecodingNeeded();
        qDebug() << "Pause state checked: resuming decoding";
    }
}

// ============================================================================
// receiveChangeProgress - 接收用户拖动进度条的位置
// ============================================================================
void VideoDecoder::receiveChangeProgress(int64_t change){
    m_currentProcess = change;
}

// ============================================================================
// receiveFrameDisplayCost - 接收 UI 线程报告的显示耗时
// ============================================================================
// 使用滑动窗口（10 帧）平滑显示耗时，用于帧同步延迟补偿
// ============================================================================
void VideoDecoder::receiveFrameDisplayCost(int costMs) {
    if(m_displayCostVector.size() < 10){
        m_displayCostVector.append(costMs);
    } else {
        m_displayCostVector.pop_front();
        m_displayCostVector.append(costMs);
    }
    int sum = 0;
    for(int i = 0; i < m_displayCostVector.size(); ++i){
        sum += m_displayCostVector[i];
    }
    m_lastDisplayCost = sum / m_displayCostVector.size();
}

// ============================================================================
// receiveFrameDisplayFinished - 接收 UI 线程报告的帧显示完成时间
// ============================================================================
void VideoDecoder::receiveFrameDisplayFinished(int64_t endTime) {
    m_frameDisplayEndTime = endTime;
}

// ============================================================================
// getCurrentTime - 获取当前帧的理论播放时间（毫秒）
// ============================================================================
// 用于帧同步和 UI 进度条更新
// 优先使用 frame 的 PTS，回退到 packet 的 PTS
// ============================================================================
void VideoDecoder::getCurrentTime(){
    int64_t currentMs = 0;
    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIndex];
    if (m_frame->pts != AV_NOPTS_VALUE) {
        currentMs = av_rescale_q(m_frame->pts, videoStream->time_base, {1, 1000});
    } else if (m_packet->pts != AV_NOPTS_VALUE) {
        currentMs = av_rescale_q(m_packet->pts, videoStream->time_base, {1, 1000});
    }
    if (currentMs != AV_NOPTS_VALUE) {
        emit sendCurrentProgress(currentMs);  // 更新 UI 进度条
    }
    m_currentTheoryMs = currentMs;  // 保存理论时间（用于全局偏差校准）
}
