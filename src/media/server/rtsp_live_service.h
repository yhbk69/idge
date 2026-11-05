#ifndef RTSP_LIVE_SERVICE_H
#define RTSP_LIVE_SERVICE_H

// ============================================================================
// RtspLiveService — 把"已画检测框"的视频作为 RTSP 直播流对外发布
// ----------------------------------------------------------------------------
// 背景：§6.1 `/api/streams` 原本把配置里的视频源地址（rtsp://摄像头/本地文件/…）
// 原样回给 Pad，Pad 播到的是**无标注**的原始流。需求是 Pad 要看带框的画面。
//
// 链路（全硬件、零额外编码）：
//   显示侧 RGBA DMA-BUF（已画框）
//     → VideoRecorder.rkmpp 硬编码（HEVC/H.264 Annex-B）   ← 报警片段复用同一个编码器
//     → 本服务的 onPacket 回调                             ← 本文件接入点
//     → mk_media_input_h264/h265 喂进 ZLM
//     → Pad 用 rtsp://<本机IP>:<port>/live/<streamId> 拉流
//
// 所以本服务**不做任何编码**，它只是把 VideoRecorder 已经产好的 Annex-B 包
// 转交给 ZLM 的 media source。编码器、分辨率、帧率、GOP 全都跟随 recording 段。
//
// 与 HttpFileService 同属"ZLM 内嵌服务"，共享 mk_env_init1（见 zlm_env.h）。
// 端口改了必须重启进程（mk_env_init1 进程级一次，与 HttpFileService 同约束）。
//
// streamId 口径：与 §6.1 一致，CH1..CH4 = 通道号+1。ZLM 的 app 固定 "live"，
// 所以完整地址是 rtsp://<ip>:<port>/live/CH1。
// ============================================================================

#include <QString>
#include <QtGlobal>
#include <functional>

class RtspLiveService
{
public:
    static RtspLiveService &instance();

    /// 启动 RTSP 服务器（幂等：已在跑直接返回 true）
    /// @param port 期望端口；0 = 失败
    /// @return true = 已监听
    bool start(quint16 port);

    /// 释放全部 media source（不停止 RTSP server；server 由进程退出时统一 mk_stop_all_server）
    void stop();

    quint16 port() const { return port_; }
    bool running() const { return port_ != 0; }

    /// rtsp://<第一个非回环 IPv4>:<实际绑定端口>；取不到 IP 返回空串
    QString baseUrl() const;

    /// 为某路流创建 media source（已存在则先释放再重建，用于换流）
    /// @param streamId 如 "CH1"
    /// @param codec "hevc" 或 "h264"（其余按 h264 兜底）
    void createSource(const QString &streamId, const QString &codec,
                      int width, int height, int fps, int bitRate);

    /// 喂一帧 Annex-B 码流；source 不存在则静默丢弃（解码器停了之后延迟任务还会推几帧）
    void pushH264(const QString &streamId, const uint8_t *data, int len, uint64_t pts_ms);
    void pushH265(const QString &streamId, const uint8_t *data, int len, uint64_t pts_ms);

    /// 释放某路流的 media source
    void removeSource(const QString &streamId);

    /// 某路流是否已注册（编码器 init 成功后才为 true）
    bool hasSource(const QString &streamId) const;

private:
    RtspLiveService() = default;
    ~RtspLiveService() = default;
    RtspLiveService(const RtspLiveService &) = delete;
    RtspLiveService &operator=(const RtspLiveService &) = delete;

    quint16 port_ = 0;
};

#endif  // RTSP_LIVE_SERVICE_H
