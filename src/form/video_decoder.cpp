#include "video_decoder.h"
#include <QDebug>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <QtGlobal>
extern "C"
{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libswscale/swscale.h>
    #include <libavdevice/avdevice.h>
    #include <libavformat/version.h>
    #include <libavutil/time.h>
    #include <libavutil/mathematics.h>
    #include <libavutil/imgutils.h>

}

VideoDecoder::VideoDecoder(QObject *parent) : QObject(parent)
{
    // 初始化FFmpeg组件（分配内存）
    m_packet = av_packet_alloc();
    m_frame = av_frame_alloc();
    m_rgbFrame = av_frame_alloc();

    // 初始化定时器
    m_pauseTimer = new QTimer(this);
    m_pauseTimer->setInterval(100);  // 检查间隔，可调整
    connect(m_pauseTimer, &QTimer::timeout, this, &VideoDecoder::checkPauseState);

    // 连接恢复信号到内部槽（关键！）
    connect(this, &VideoDecoder::resumeDecodingNeeded, this, [this]() {
        qDebug() << "Resuming decoding process";
        // 这里不需要额外操作，因为主循环会继续执行
    }, Qt::QueuedConnection);
}

VideoDecoder::~VideoDecoder()
{
    // 确保解码已停止
    receiveStopDecoding();

    // 释放FFmpeg资源
    freeFFmpegResources();

    // 释放帧和数据包
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

void VideoDecoder::receiveVideoPath(QString path)
{
    m_videoPath = path;

    //此处就要获取视频一些基础信息例如视频长度什么的 并且
    // 初始化FFmpeg资源
    freeFFmpegResources();  // 先释放旧资源（防止重复初始化）

    // 1. 打开视频文件并获取格式信息
    m_formatCtx = avformat_alloc_context();
    if (avformat_open_input(&m_formatCtx, m_videoPath.toUtf8().constData(), nullptr, nullptr) != 0) {
        qCritical() << "无法打开视频文件:" << m_videoPath;
        emit decodingFinished();
        return;
    }

    // 2. 获取流信息
    if (avformat_find_stream_info(m_formatCtx, nullptr) < 0) {
        qCritical() << "无法获取视频流信息";
        freeFFmpegResources();
        emit decodingFinished();
        return;
    }

    // 3. 查找视频流
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

    // 获取视频时长（以毫秒为单位）
//    int64_t duration = m_formatCtx->duration;
//    if (duration == AV_NOPTS_VALUE) {
//        qDebug() << "无法获取视频时长";
//        return;
//    }
//    int64_t totalDurationMs = duration * 1000 / AV_TIME_BASE;
//    emit sendAllProgress(totalDurationMs);

    m_totalDurationMs = m_formatCtx->duration;
    if (m_totalDurationMs == AV_NOPTS_VALUE) {
        qDebug() << "无法获取视频时长";
        return;
    }
    m_totalDurationMs = m_totalDurationMs * 1000 / AV_TIME_BASE;
    qDebug() << "视频时长ms:" << m_totalDurationMs;
    emit sendAllProgress(m_totalDurationMs);

    m_startRealTime = QDateTime::currentMSecsSinceEpoch(); // 记录开始时间

}

void VideoDecoder::receiveStartDecoding()
{
    m_count = 0;

    qint64 m_startTime = QDateTime::currentMSecsSinceEpoch();
    // 标记为正在解码（线程安全）
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

    //获取实际帧率
    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIndex];
    // 计算帧率（fps = 分子/分母）
    double fps = av_q2d(videoStream->avg_frame_rate);
    // 处理特殊情况（若avg_frame_rate无效，用r_frame_rate）
    if (fps <= 0) {
        fps = av_q2d(videoStream->r_frame_rate);
    }
    // 保存帧率（作为类成员变量 double m_fps;）
    m_fps = fps;
    qDebug()<<"m_fps:"<<m_fps;

    // 5. 发送视频原始信息到主线程（用于UI初始化）
    emit videoInfoDecoded(m_codecCtx->width, m_codecCtx->height);

    // 6. 初始化帧转换资源（RGB帧和缓冲区）
    int rgbBufferSize = av_image_get_buffer_size(
        AV_PIX_FMT_RGB32,    // 目标格式（匹配QImage::Format_RGB32）
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

    //av_find_best_stream();
    // 7. 解码主循环
    while (true) {
        m_frameTimer.start();

        // 处理待处理的事件（确保信号被接收）
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);  // 处理10ms内的事件
        // 检查是否需要停止解码（线程安全）
        {
            QMutexLocker locker(&m_stateMutex);
            if (!m_isDecoding) break;
        }

        //**********************暂停视频的代码↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓
        // 检查是否需要暂停
        bool shouldPause = false;
        {
            QMutexLocker pauseLocker(&m_pauseMutex);
            shouldPause = m_isPaused;
        }
        if (shouldPause) {
            qDebug() << "Decoder paused, waiting for resume...";
        //↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓持续检查暂停状态，直到恢复↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓↓
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
                QThread::msleep(50);  // 降低CPU占用
            }
        }
        //↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑暂停视频的代码↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑↑
        // 读取一帧数据
        int ret = av_read_frame(m_formatCtx, m_packet);
        if (ret < 0) {
            // 读取完毕（正常结束）
            break;
        }

        // 只处理视频流
        if (m_packet->stream_index == m_videoStreamIndex) {
            // 发送数据包到解码器
            if (avcodec_send_packet(m_codecCtx, m_packet) < 0) {
                qWarning() << "发送数据包到解码器失败";
                av_packet_unref(m_packet);
                continue;
            }

            // 循环接收解码后的帧（可能有多个）
            while (true) {
                //qint64 oneFrameTime = QDateTime::currentMSecsSinceEpoch();

                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    // 需要更多数据或解码结束，退出当前循环
                    break;
                } else if (ret < 0) {
                    qWarning() << "接收解码帧失败";
                    break;
                }
                //调用获取当前时间戳
                getCurrentTime();
                m_frameStartTime = QDateTime::currentMSecsSinceEpoch(); // 记录当前帧开始时间
                if(m_swsCtx == nullptr){
                    m_swsCtx = sws_getContext(
                                m_codecCtx->width,          // 源宽度（原始尺寸）
                                m_codecCtx->height,         // 源高度（原始尺寸）
                                m_codecCtx->pix_fmt,        // 源格式
                                m_codecCtx->width,          // 目标宽度（保持原始）
                                m_codecCtx->height,         // 目标高度（保持原始）
                                AV_PIX_FMT_RGB32,           // 目标格式（匹配QImage）
                                SWS_BILINEAR,
                                nullptr, nullptr, nullptr
                                );
                }

                if (!m_swsCtx) {
                    qWarning() << "无法创建图像转换上下文";
                    av_frame_unref(m_frame);
                    continue;
                }

                // 执行缩放和格式转换
                sws_scale(
                    m_swsCtx,
                    (const unsigned char* const*)m_frame->data,  // 源数据
                    m_frame->linesize,                          // 源行跨度
                    0,                                          // 起始行
                    m_codecCtx->height,                         // 高度
                    m_rgbFrame->data,                           // 目标数据
                    m_rgbFrame->linesize                         // 目标行跨度
                );

                // 10. 转换为QImage并发送到主线程
                QImage frame(
                        m_rgbFrame->data[0],
                        m_codecCtx->width,
                        m_codecCtx->height,
                        m_rgbFrame->linesize[0],
                        QImage::Format_RGB32
                        );
                // 发送副本（避免多线程访问同一内存）
                m_frameSendTime = QDateTime::currentMSecsSinceEpoch();
                emit frameDecoded(frame);
                //emit frameDecodedWithTime(frame,QDateTime::currentMSecsSinceEpoch());
                qDebug() <<"send time        "<<m_count<< ": "
                << QDateTime::fromMSecsSinceEpoch(QDateTime::currentMSecsSinceEpoch()).toLocalTime().toString("hh:mm:ss.zzz");
                //<<;QDateTime::currentMSecsSinceEpoch() ;
                m_count++;

                //qint64 oneFrameEndTime = QDateTime::currentMSecsSinceEpoch() - oneFrameTime;
                //解码一帧需要4-5 毫秒
                //qDebug() <<m_count<< " : " << oneFrameEndTime ;
                //m_count++;
                // 基于时间戳的同步 同步控制会休息大概一个浮动的29-33毫秒
                //qint64 time1 = QDateTime::currentMSecsSinceEpoch();
                if (m_frame->pts != AV_NOPTS_VALUE) {
                    if (m_lastFramePts != AV_NOPTS_VALUE) {
                        /*方法1同步时间不行
                        double frameDelay = av_q2d(videoStream->time_base) *
                                (m_frame->pts - m_lastFramePts) * 1000;

                        // 确保合理的帧间隔(5ms-1000ms)
                        //frameDelay = qBound(5.0, frameDelay, 1000.0);
                        //这个-10 是因为主线程缩放图片需要大约10ms
                        frameDelay = qBound(0.0, frameDelay , 1000.0);

                        //qDebug() << "frameDelay1:"<<frameDelay;
                        frameDelay -= (m_lastDisplayCost > 0? m_lastDisplayCost: 9);
                        //qDebug() << "frameDelay2:"<<frameDelay;
                        qDebug() <<"m_lastDisplayCost:"<<m_lastDisplayCost;
                        int elapsed = m_frameTimer.restart();
                        int remainingDelay = frameDelay - elapsed;
                        if (remainingDelay > 0) {
                            QThread::msleep(remainingDelay);
                        }
                        */

                        // 方法2同步时间较接近
                        // 1. 计算单帧理论间隔
                        double frameDelay = av_q2d(videoStream->time_base) *
                                (m_frame->pts - m_lastFramePts) * 1000;
                        frameDelay = qBound(0.0, frameDelay, 1000.0);
                        // 2. 减去上一帧的显示耗时（单帧补偿）
                        frameDelay -= (m_lastDisplayCost > 0 ? m_lastDisplayCost : 9);
                        // 3. 计算全局时间偏差（核心校准）
                        if (m_startRealTime != -1 && m_currentTheoryMs >= 0) {
                            // 实际流逝的系统时间（从播放开始到现在）
                            qint64 realElapsed = QDateTime::currentMSecsSinceEpoch() - m_startRealTime;
                            // 理论应流逝的时间（当前帧的理论播放时间）
                            qint64 theoryElapsed = m_currentTheoryMs;
                            // 偏差 = 理论时间 - 实际时间（正数表示播放偏快，需增加延迟；负数表示偏慢，需减少延迟）
                            int globalDiff = theoryElapsed - realElapsed;
                            // 4. 用全局偏差微调当前帧的延迟（限制单次调整幅度，避免突变）
                            frameDelay += qBound(-5, globalDiff / 5, 5); // 每次最多调整±5ms
                        }
                        // 5. 最终延迟计算
                        int elapsed = m_frameTimer.restart();
                        int remainingDelay = frameDelay - elapsed;
                        remainingDelay = qBound(0, remainingDelay, 100); // 限制最大延迟，避免卡顿
                        qDebug()<<"remainingDelay:"<<remainingDelay;
                        if (remainingDelay > 0) {
                            QThread::msleep(remainingDelay);
                        }


                        /*
                        //方法3 同步时间不行
                        // 1. 计算理论间隔（两帧PTS差）
                        double frameDelay = av_q2d(videoStream->time_base) *
                                (m_frame->pts - m_lastFramePts) * 1000;
                        frameDelay = qBound(0.0, frameDelay, 1000.0);

                        // 2. 计算当前帧的实际已用时间（从解码开始到现在）
                        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
                        qint64 elapsedTotal = currentTime - m_frameStartTime; // 已用时间（解码+缩放反馈耗时）

                        // 3. 计算需要休眠的时间（确保总耗时=理论间隔）
                        int needSleep = frameDelay - elapsedTotal  - m_lastDisplayCost;
                        needSleep = qBound(0, needSleep, 100); // 最多休眠100ms，避免异常
                        qDebug()<<"needSleep:"<<needSleep;
                        // 4. 执行休眠
                        if (needSleep > 0) {
                            QThread::msleep(needSleep);
                        }

                        // 调试日志：验证总耗时是否接近理论值
                        //qint64 totalCost = QDateTime::currentMSecsSinceEpoch() - m_frameStartTime;
                        //qDebug() << "理论间隔:" << frameDelay << "ms，实际总耗时:" << totalCost << "ms";
                        */

                        /*
                        //方法4
                        // 1. 理论间隔（不变）
                        double frameDelay = av_q2d(videoStream->time_base) *
                                (m_frame->pts - m_lastFramePts) * 1000;
                        frameDelay = qBound(0.0, frameDelay, 1000.0);

                        // 2. 等待主线程缩放完成（关键：确保获取到缩放结束时间）
                        while (m_frameDisplayEndTime == -1) {
                            QThread::msleep(1); // 等待主线程反馈，最多等50ms
                            if (QDateTime::currentMSecsSinceEpoch() - m_frameSendTime > 50) {
                                m_frameDisplayEndTime = QDateTime::currentMSecsSinceEpoch(); // 超时 fallback
                                break;
                            }
                        }
                        // 3. 计算全流程总耗时（解码开始→缩放完成）
                        int64_t totalCost = m_frameDisplayEndTime - m_frameStartTime;
                        qDebug()<<"totalCost:"<<totalCost;
                        // 4. 计算需要休眠的时间（确保总耗时=理论间隔）
                        int needSleep = frameDelay - totalCost;
                        needSleep = qBound(0, needSleep, 100); // 限制范围
                        // 5. 执行休眠（当前帧的全流程已结束，休眠为下一帧做准备）
                        if (needSleep > 0) {
                            QThread::msleep(needSleep);
                        }
                        // 重置变量，为下一帧准备
                        m_frameDisplayEndTime = -1;
                        //qDebug()<<"sleep time:"<<needSleep;
                        */
                    }
                    m_lastFramePts = m_frame->pts;
                } else {
                    // 无时间戳时使用固定帧率
                    QThread::msleep(qMax(1, static_cast<int>(1000.0 / m_fps)));
                }

                //qint64 time2 = QDateTime::currentMSecsSinceEpoch() - time1;
                //qDebug() <<m_count<< " : " << time2 ;
                //m_count++;

            }
        }

        // 释放数据包（必须在循环内释放，否则内存泄漏）
        av_packet_unref(m_packet);
    }
    qint64 totalPlayTime = QDateTime::currentMSecsSinceEpoch() - m_startTime; // m_startTime在receiveStartDecoding()开始时记录
    qDebug() << "实际播放耗时(s):" << totalPlayTime ;//<< "，视频总时长(ms):" << totalDurationMs;
    // 12. 解码结束清理
    av_packet_unref(m_packet);  // 确保最后一个包被释放
    freeFFmpegResources();
    emit decodingFinished();
}

void VideoDecoder::receiveStopDecoding()
{
    QMutexLocker locker(&m_stateMutex);
    m_isDecoding = false;
}

void VideoDecoder::freeFFmpegResources()
{

    // 释放缩放上下文
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }

    // 释放RGB缓冲区
    if (m_rgbBuffer) {
        av_free(m_rgbBuffer);
        m_rgbBuffer = nullptr;
    }

    // 释放解码器上下文（新版本：直接调用avcodec_free_context）
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }

    // 释放格式上下文
    if (m_formatCtx) {
        avformat_close_input(&m_formatCtx);
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
    }

    // 重置解码器和解码状态
    m_codec = nullptr;

}

void VideoDecoder::receivePauseDecoding(){
    qDebug()<<"receivePauseDecoding";
    QMutexLocker locker(&m_pauseMutex);
    if (!m_isPaused) {
        m_isPaused = true;
        qDebug() << "Pause signal received, decoder paused";
    }
}

void VideoDecoder::receiveResumeDecoding(){
    qDebug() <<"receiveResumeDecoding";
    //return;
    QMutexLocker locker(&m_pauseMutex);
    if (m_isPaused) {
        m_isPaused = false;
        qDebug() << "Resume signal received, decoder will resume";
        // 不需要手动唤醒，由定时器检查状态变化
    }
}
// 检查暂停状态的槽函数
void VideoDecoder::checkPauseState() {
    QMutexLocker locker(&m_pauseMutex);
    if (!m_isPaused && m_pauseTimer->isActive()) {
        m_pauseTimer->stop();
        emit resumeDecodingNeeded();  // 触发恢复解码的信号
        qDebug() << "Pause state checked: resuming decoding";
    }
}

void VideoDecoder::receiveChangeProgress(int64_t change){
    m_currentProcess = change;
    //qDebug()<<m_currentTime;
}

void VideoDecoder::receiveFrameDisplayCost(int costMs) {

    //m_lastDisplayCost = costMs; // 实时更新最新耗时

    if(m_displayCostVector.size()<10){
        m_displayCostVector.append(costMs);
    }else{
        m_displayCostVector.pop_front();
        m_displayCostVector.append(costMs);
    }

    int sum = 0;
    for(int i = 0;i<m_displayCostVector.size();++i){
        sum += m_displayCostVector[i];
    }
    m_lastDisplayCost = sum / m_displayCostVector.size();

}

void VideoDecoder::receiveFrameDisplayFinished(int64_t endTime) {
        m_frameDisplayEndTime = endTime; // 接收主线程的完成时间
    }
void VideoDecoder::getCurrentTime(){

    int64_t currentMs = 0;
    // 获取视频流（用于时间基转换）
    AVStream* videoStream = m_formatCtx->streams[m_videoStreamIndex];
    if (m_frame->pts != AV_NOPTS_VALUE) {
        // 直接转换为毫秒，避免多步计算
        currentMs = av_rescale_q(m_frame->pts, videoStream->time_base, {1, 1000});
    } else if (m_packet->pts != AV_NOPTS_VALUE) {
        currentMs = av_rescale_q(m_packet->pts, videoStream->time_base, {1, 1000});
    }
    //考虑到有些视频开头可能有负时间戳，这里可以调整判断条件
    //某视频从 -500ms 开始有数据，到 0ms 时才是用户感知的 “开始”，负时间戳的帧可能是预加载的关键帧或音频前置数据。
    //音视频同步调整：为了让音频和视频同步，有时会对其中一方的时间戳进行偏移。例如，若视频比音频晚 100ms，可能将音频时间戳减去 100ms，导致部分音频帧出现负值。
    if (currentMs != AV_NOPTS_VALUE) {
        emit sendCurrentProgress(currentMs);
    }
    m_currentTheoryMs = currentMs;

}