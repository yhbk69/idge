#ifndef VIDEO_RECORDER_H
#define VIDEO_RECORDER_H

// ============================================================================
// VideoRecorder - 视频录制器（环形缓冲区）
// ============================================================================
//
// 作用：
//   持续编码视频帧并存储在内存环形缓冲区中。
//   当报警触发时，将缓冲区内容 dump 到 MP4 文件。
//
// 设计：
//   - 使用 FFmpeg 软件编码器将 RGBA 帧编码为 H.264
//   - 环形缓冲区存储压缩后的 AVPacket（比原始帧小 100+ 倍）
//   - 30秒 × 25fps × 50KB/帧 ≈ 37MB 内存占用
//
// 使用：
//   1. 构造时指定缓冲区时长（秒）和帧率
//   2. 每帧调用 encodeFrame() 编码并存入缓冲区
//   3. 报警时调用 dumpToFile() 写入 MP4
//
// ============================================================================

#include <QObject>
#include <QString>
#include <QVector>
#include <QMutex>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class VideoRecorder : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief 构造函数
     * @param bufferSeconds: 环形缓冲区时长（秒），默认 30 秒
     * @param fps: 帧率，默认 25
     * @param parent: 父对象
     */
    explicit VideoRecorder(int bufferSeconds = 30, int fps = 25, QObject *parent = nullptr);
    ~VideoRecorder();

    /**
     * @brief 初始化编码器
     * @param width: 视频宽度
     * @param height: 视频高度
     * @return: true=成功，false=失败
     */
    bool init(int width, int height);

    /**
     * @brief 编码一帧并存入环形缓冲区
     * @param rgbaData: RGBA8888 像素数据
     * @param width: 宽度
     * @param height: 高度
     * @param timestamp_ms: 时间戳（毫秒）
     *
     * @note 在解码线程中调用，内部会做 RGBA→YUV420 转换
     */
    void encodeFrame(const unsigned char *rgbaData, int width, int height, qint64 timestamp_ms);

    /**
     * @brief 将环形缓冲区内容 dump 到 MP4 文件
     * @param filePath: 输出文件路径
     * @return: true=成功，false=失败
     *
     * @note 包含报警前 bufferSeconds 秒的视频
     */
    bool dumpToFile(const QString &filePath);

    /**
     * @brief 是否已初始化
     */
    bool isInitialized() const { return initialized_; }

    /**
     * @brief 获取缓冲区中的帧数
     */
    int bufferedFrameCount() const;

    /**
     * @brief 获取缓冲区时长（秒）
     */
    int bufferSeconds() const { return bufferSeconds_; }

private:
    /**
     * @brief 编码一帧为 H.264 NAL 单元
     */
    bool encodePacket(const unsigned char *rgbaData, int width, int height, qint64 pts);

    /**
     * @brief 将编码后的包存入环形缓冲区
     */
    void bufferPacket(AVPacket *pkt, qint64 pts);

    // 配置
    int bufferSeconds_;     ///< 环形缓冲区时长（秒）
    int fps_;               ///< 帧率
    int width_ = 0;         ///< 视频宽度
    int height_ = 0;        ///< 视频高度

    // 编码器
    AVCodecContext *codecCtx_ = nullptr;  ///< H.264 编码器上下文
    SwsContext *swsCtx_ = nullptr;        ///< RGBA→YUV420 转换上下文
    AVFrame *yuvFrame_ = nullptr;         ///< YUV420 帧缓冲
    bool initialized_ = false;

    // 环形缓冲区
    struct BufferedPacket {
        std::vector<uint8_t> data;    ///< 压缩后的 H.264 数据
        qint64 pts;                   ///< 显示时间戳
        bool isKeyFrame;              ///< 是否为关键帧
    };

    std::vector<BufferedPacket> ringBuffer_;  ///< 环形缓冲区
    int maxFrames_;                           ///< 最大帧数
    mutable QMutex mutex_;                    ///< 保护缓冲区的互斥锁
};

#endif // VIDEO_RECORDER_H
