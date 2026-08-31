#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <QSize>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>
#include <QElapsedTimer>
// FFmpeg头文件
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
class VideoDecoder : public QObject
{
    Q_OBJECT
public:

    explicit VideoDecoder(QObject *parent = nullptr);

    ~VideoDecoder();


public slots:
    // 开始解码（子线程中执行）
    void receiveStartDecoding();

    // 停止解码（线程安全）
    void receiveStopDecoding();

    // 设置视频文件路径
    void receiveVideoPath(QString path);

    void receivePauseDecoding();

    void receiveResumeDecoding();

    void checkPauseState();  // 检查暂停状态的槽

    void receiveFrameDisplayCost(int costMs); // 接收主线程耗时

    void receiveFrameDisplayFinished(int64_t endTime); // 接收主线程的完成时间
    //接受用户变化的进度
    void receiveChangeProgress(int64_t change);


signals:
    // 发送解码后的视频帧（QImage格式）到主线程
    void frameDecoded(QImage frame);

    void frameDecodedWithTime(QImage frame,qint64 time);

    // 发送视频原始信息（宽高）到主线程
    void videoInfoDecoded(int originalWidth, int originalHeight);

    // 解码结束信号（用于通知主线程清理资源）
    void decodingFinished();

    void resumeDecodingNeeded();  // 触发恢复解码的信号

    //发送当前播放进度
    void sendCurrentProgress(int64_t currentTime);

    //发送视频时长
    void sendAllProgress(int64_t allTime);

private:
    // 释放FFmpeg相关资源
    void freeFFmpegResources();

    // 视频文件路径
    QString m_videoPath;

    // 目标显示尺寸（窗口大小）
    QSize m_targetSize;
    QMutex m_targetSizeMutex;  // 保护m_targetSize的线程安全访问

    // 解码状态标记（线程安全）
    volatile bool m_isDecoding;
    QMutex m_stateMutex;

    // FFmpeg核心组件
    AVFormatContext *m_formatCtx = nullptr;    // 格式上下文
    AVCodecContext *m_codecCtx = nullptr;      // 编解码器上下文
    const AVCodec *m_codec = nullptr;                // 解码器
    AVFrame *m_frame = nullptr;                // 原始帧（解码后）
    AVFrame *m_rgbFrame = nullptr;             // 转换为RGB的帧（用于QImage）
    AVPacket *m_packet = nullptr;              // 数据包
    SwsContext *m_swsCtx = nullptr;            // 图像转换上下文
    unsigned char *m_rgbBuffer = nullptr;      // RGB帧的缓冲区
    double m_fps;//实际帧率
    int m_currentBufferWidth = 0; int m_currentBufferHeight = 0;

    QElapsedTimer m_frameTimer;
    int64_t m_lastFramePts = AV_NOPTS_VALUE;
    //0是关闭 继续是播放 2是暂停
    std::atomic<int> m_playState;

    QMutex m_pauseMutex;          // 保护暂停状态的互斥锁
    QWaitCondition m_pauseCondition;  // 暂停等待条件
    bool m_isPaused = false;      // 暂停状态标记（需通过m_pauseMutex访问）

    QTimer* m_pauseTimer;
    int m_videoStreamIndex = -1;
    //获取当前视频播放进度
    void getCurrentTime();

    int m_count = 0;

    int64_t m_currentProcess = 0;

    int m_lastDisplayCost = 0; // 记录上一帧的显示耗时
    //用于记录最新的10个解析图片的平均消耗
    QVector<int> m_displayCostVector ;
    int m_displayCostVectorIndex = -1;
    int64_t m_startRealTime = -1; // 播放开始的实际系统时间（毫秒）
    int64_t m_totalDurationMs = 0;
    int64_t m_currentTheoryMs = 0;

    int64_t m_frameStartTime = -1; // 记录当前帧解码开始的系统时间

    int64_t m_frameSendTime = 0; //发送一帧到主线程的时间
    int64_t m_frameDisplayEndTime = -1;//接收到主线程缩放播放一帧后的时间
};

#endif // VIDEODECODER_H