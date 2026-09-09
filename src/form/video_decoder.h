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
// FFmpeg头文件 - 用于视频解码和格式转换
extern "C" {
#include <libavformat/avformat.h>  // 格式处理：打开、读取、关闭媒体文件
#include <libavcodec/avcodec.h>    // 编解码：解码视频帧
#include <libswscale/swscale.h>    // 图像缩放：转换颜色空间和分辨率
}

/* 
====================================================
作用：视频解码器类 - 负责从视频文件中解码帧数据
说明：在独立线程中运行，通过Qt信号槽机制与主线程通信
====================================================
*/
class VideoDecoder : public QObject
{
    Q_OBJECT
public:

    /* 
    ====================================================
    作用：构造函数
    说明：初始化视频解码器，设置默认参数
    参数：parent - 父对象指针（Qt对象树机制）
    ====================================================
    */
    explicit VideoDecoder(QObject *parent = nullptr);

    /* 
    ====================================================
    作用：析构函数
    说明：释放FFmpeg资源和系统内存
    ====================================================
    */
    ~VideoDecoder();


public slots:
    /* 
    ====================================================
    作用：开始解码槽函数
    说明：在子线程中执行，启动视频解码循环
    说明：通过信号槽机制从主线程触发，实现线程间通信
    ====================================================
    */
    void receiveStartDecoding();

    /* 
    ====================================================
    作用：停止解码槽函数
    说明：线程安全地停止解码过程，释放相关资源
    说明：使用互斥锁保护状态变量，确保线程安全
    ====================================================
    */
    void receiveStopDecoding();

    /* 
    ====================================================
    作用：设置视频文件路径槽函数
    说明：接收用户选择的视频文件路径，准备解码
    参数：path - 视频文件的完整路径
    ====================================================
    */
    void receiveVideoPath(QString path);

    /* 
    ====================================================
    作用：暂停解码槽函数
    说明：暂停视频解码，保持当前状态
    ====================================================
    */
    void receivePauseDecoding();

    /* 
    ====================================================
    作用：恢复解码槽函数
    说明：从暂停状态恢复视频解码
    ====================================================
    */
    void receiveResumeDecoding();

    /* 
    ====================================================
    作用：检查暂停状态槽函数
    说明：定时检查暂停状态，实现暂停/恢复逻辑
    ====================================================
    */
    void checkPauseState();  // 检查暂停状态的槽

    /* 
    ====================================================
    作用：接收帧显示耗时槽函数
    说明：接收主线程处理一帧的耗时，用于同步控制
    参数：costMs - 显示一帧消耗的毫秒数
    ====================================================
    */
    void receiveFrameDisplayCost(int costMs); // 接收主线程耗时

    /* 
    ====================================================
    作用：接收帧显示完成时间槽函数
    说明：接收主线程完成显示的时间戳，用于同步
    参数：endTime - 显示完成的系统时间戳
    ====================================================
    */
    void receiveFrameDisplayFinished(int64_t endTime); // 接收主线程的完成时间
    
    /* 
    ====================================================
    作用：接收进度变化槽函数
    说明：处理用户拖动进度条的操作，跳转到指定时间点
    参数：change - 目标时间点（毫秒）
    ====================================================
    */
    void receiveChangeProgress(int64_t change); // 接受用户变化的进度


signals:
    /* 
    ====================================================
    作用：发送解码帧信号
    说明：将解码后的视频帧发送到主线程进行显示
    参数：frame - 解码后的图像帧（QImage格式）
    ====================================================
    */
    void frameDecoded(QImage frame);

    /* 
    ====================================================
    作用：发送带时间戳的解码帧信号
    说明：发送帧数据时附带时间戳，用于音视频同步
    参数：frame - 图像帧，time - 帧的时间戳
    ====================================================
    */
    void frameDecodedWithTime(QImage frame,qint64 time);

    /* 
    ====================================================
    作用：发送视频原始信息信号
    说明：通知主线程视频的原始分辨率，用于窗口调整
    参数：originalWidth - 原始宽度，originalHeight - 原始高度
    ====================================================
    */
    void videoInfoDecoded(int originalWidth, int originalHeight);

    /* 
    ====================================================
    作用：解码结束信号
    说明：通知主线程解码完成，可以清理资源
    ====================================================
    */
    void decodingFinished();

    /* 
    ====================================================
    作用：触发恢复解码信号
    说明：从暂停状态恢复时发送此信号
    ====================================================
    */
    void resumeDecodingNeeded();  // 触发恢复解码的信号

    /* 
    ====================================================
    作用：发送当前播放进度信号
    说明：定时发送当前播放位置，用于进度条更新
    参数：currentTime - 当前播放时间（毫秒）
    ====================================================
    */
    void sendCurrentProgress(int64_t currentTime);

    /* 
    ====================================================
    作用：发送视频总时长信号
    说明：发送视频的总时长，用于进度条初始化
    参数：allTime - 视频总时长（毫秒）
    ====================================================
    */
    void sendAllProgress(int64_t allTime);

private:
    /* 
    ====================================================
    作用：释放FFmpeg资源
    说明：清理所有FFmpeg相关的内存和上下文
    ====================================================
    */
    void freeFFmpegResources();

    // 视频文件路径
    QString m_videoPath;

    // 目标显示尺寸（窗口大小）
    QSize m_targetSize;
    QMutex m_targetSizeMutex;  // 保护m_targetSize的线程安全访问

    // 解码状态标记（线程安全）
    volatile bool m_isDecoding;
    QMutex m_stateMutex;

    /* 
    ====================================================
    FFmpeg核心组件说明：
    - AVFormatContext: 格式上下文，管理媒体文件的整体信息
    - AVCodecContext: 编解码器上下文，存储编解码参数
    - AVCodec: 解码器实例，执行实际的解码操作
    - AVFrame: 帧数据，存储解码后的图像数据
    - AVPacket: 数据包，存储压缩的视频数据
    - SwsContext: 图像转换上下文，用于颜色空间转换
    ====================================================
    */
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

    // 帧计时器，用于计算帧间隔
    QElapsedTimer m_frameTimer;
    int64_t m_lastFramePts = AV_NOPTS_VALUE;
    //播放状态：0是关闭，1是继续播放，2是暂停
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