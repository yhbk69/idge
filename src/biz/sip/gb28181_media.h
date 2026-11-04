#ifndef IDGE_GB28181_MEDIA_H
#define IDGE_GB28181_MEDIA_H

/**
 * @file gb28181_media.h
 * @brief 国标推流的取流/转码源：一路通道 = 一条工作线程，产出 Annex-B H.264 帧
 */

#include "gb28181_config.h"

#include <QByteArray>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/// 交给 PsMuxer 的一帧：已是 Annex-B（含起始码），pts 为 90kHz 时基
struct Gb28181EncodedFrame {
    std::vector<uint8_t> data;
    uint64_t pts90k = 0;
    bool keyFrame = false;
};

/**
 * @brief 单路国标媒体源（非 QObject，回调在工作线程上执行）
 *
 * 取流定义来自 `video.sources[]`（`Gb28181StreamConfig::takeSource()`），本类只认
 * 解析后的那一份：源类型见 `take_.type` = camera(v4l2/MJPEG)、rtsp(强制 tcp)、rtmp、
 * file(旧称 mp4)。源本身是 H.264 时只过 h264_mp4toannexb 直通；否则 rkmpp 硬解
 * （失败退软解）→ swscale → h264_rkmpp 硬编（失败退 libx264）。详见 .cpp 文件头。
 */
class Gb28181MediaSource {
public:
    using FrameCallback = std::function<void(const Gb28181EncodedFrame &)>;
    using ErrorCallback = std::function<void(const QString &)>;

    explicit Gb28181MediaSource(const Gb28181StreamConfig &config);
    ~Gb28181MediaSource();

    /**
     * @brief 起工作线程开始取流
     * @return 恒为 true——开流失败是**异步**的，只会走 errorCallback 并把 running_
     *         置回 false，调用方要判成败得看 running() 或错误回调，不能看返回值。
     *
     * 已在运行时直接返回 true 不重起；重复调用无副作用。
     */
    bool start(FrameCallback frameCallback, ErrorCallback errorCallback);

    /// 置停 + join 工作线程，返回后回调保证不再被调用
    void stop();

    /// 只置停标志、不 join：给"发送侧发现会话已废"这种不能阻塞的场合用
    void requestStop() { running_ = false; }
    bool running() const { return running_.load(); }

private:
    Gb28181StreamConfig config_;   ///< 国标身份 + 推流码率（不含取流字段）
    VideoSourceCfg take_;          ///< 每次 run() 开头现解析的取流定义，宽高帧率已补回国标默认值
    FrameCallback frameCallback_;
    ErrorCallback errorCallback_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    void run();
    void reportError(const QString &message) const;
};

#endif
