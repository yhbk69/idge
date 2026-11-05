#ifndef PAD_HTTP_API_H
#define PAD_HTTP_API_H

// ============================================================================
// pad_http_api.h — v1 HTTP `/api/` 面的装配入口与各节路由的公共上下文
// ============================================================================
//
// 分文件的方式：一个章节一个 .cpp（§4 报警 / §5 围栏 / §6 视频 / §7 设备），
// 每个都只暴露一个 `registerXxxRoutes(server, ctx)`。
// 为什么用"注册路由"而不是"一个巨大的 handle()"：
//   十四个接口的分支写在一起，改一个动一片，而且自测没法只跑某一节。
//
// ctx 由装配处（src/ui/form/frmmain.cpp）从 config.json 拼好传进来，
// 路由自己**不读 ConfigManager 之外**的配置来源：
// 端口、开关、设备 ID、媒体前缀全在那一处决定，路由只消费。
// 唯一例外是 §6 视频路由要读 `video.sources`/`video.channels` 那张注册表
// ——那是"现在这一路开着什么流"的事实源，实时读才对得上界面改完之后的状态。
//
// 依赖方向：biz/pad → biz/alarm、biz/db、biz/geofence、biz/ws(仅 mediaUrlFor)、
// ai/model_repo、base，不 include src/ui。
// ============================================================================

#include <QString>

#include <functional>

#include "pad_http_server.h"

namespace pad {

/// 一次装配的全部外部条件。空串的含义见各字段注释，**不要**在路由里再兜一层默认值。
struct HttpApiContext {
    QString deviceId;                 ///< §7.1 的 device_id。空 = 路由不申报该字段（装配处应已回落 defaultDeviceId()）
    QString mediaBaseUrl;             ///< 8090 那台文件服务的地址前缀。**空 = 所有 URL 出不了**，
                                      ///  此时证据/围栏画面一律不给地址（宁缺勿错，与 pad_wire 同一口径）
    QString mediaRootDir;             ///< 与 mediaBaseUrl 对应的那棵树（绝对路径），URL 换算与落盘校验的基准
    QString fenceFrameDir;            ///< 围栏合成画面的落盘子目录（相对 mediaRootDir，例 "fence-frames"）；
                                      ///  空 = 不渲染画面，apply 会回 503/3003 而不是给一个假 URL
    bool exposeStreamCredentials = false;  ///< §6.1 的 liveUrl 是否保留 `user:pass@`。
                                      ///  **默认 false**：处置/围栏是写面，把摄像头口令整份发给局域网
                                      ///  上任何一台设备是另一回事，和"这台设备能收报警"不同级。
                                      ///  关掉时 Pad 拿到的是去凭据的地址（多半播不起来，但口令不出门）。
    std::function<bool(int channel)> channelOnline;   ///< 这一路现在有没有在出帧。
                                      ///  空函数 = 一律按 OFFLINE 报（离线自测就是这么跑的）
};

/// 注册 §4~§7 的全部路由。**先清空**再注册，所以可以随「保存并应用」反复调。
/// ctx 按值捕获进各个 handler，改配置要生效必须重新装配（frmMain 的 startPadHttpFromConfig 就是干这个的）。
void registerPadHttpRoutes(PadHttpServer *server, const HttpApiContext &ctx);

// ============================================================================
// 通道身份：§6 的 streams[] 与 §7.1 的 cameras[] 共用的唯一一份口径
// ============================================================================
// 为什么必须共用：v1 让 Pad 用 `cameraId` 把围栏绑到摄像头上、用 `streamId` 绑播放器
// （§5.2 的围栏对象同时带这两个键）。两个接口各算一次的话，
// 同一物理通道在两份响应里可能对上不同的 ID，Pad 那边就是"围栏画在另一路画面上"。
// 判据只有一条来源：config.json 的 `video.channels[]` + `video.sources[]` 注册表
// （经 ConfigManager::resolveChannelSource 那一层解析，界面/解码/这里同一口径）。
struct ChannelIdentity {
    QString streamId;      ///< 稳定的流 ID：**通道号**派生（CH1..CH4）。
                           ///  不用注册表 id 当它，是因为两路通道可以引用同一条流，
                           ///  那样两个播放器/两条围栏会撞在同一个 streamId 上
    QString cameraId;      ///< 摄像头 ID：注册表条目 id；这路没引用注册表时与 streamId 同值
    QString name;          ///< 通道名称（通道备注 > 流备注 > "通道N"）
    QString liveUrl;       ///< 能给对端播的地址；不可播时是空串（见 playable 那条）
    QString liveProtocol;  ///< RTSP / HLS / HTTP / RTMP；playable=false 时为空
    bool enabled = false;  ///< 这一路在配置里被启用了吗（video.channels[].enabled）
    bool playable = false; ///< 这个地址**局域网里的另一台设备**取得到吗
};

/// 按通道号（0-based）算出上面那三个身份 + 流地址。exposeCredentials 见 HttpApiContext。
ChannelIdentity channelIdentity(int channel, bool exposeCredentials);

/**
 * @brief 这一路现在有没有在出帧（§6 的 status 与 §7.1 的 cameras[].online 共用）
 * @return ctx.channelOnline 没装配时**一律 false**
 *
 * 判据由装配处注入（frmMain 把它接到 AlarmManager 的通道在线表），为的是让路由
 * 不依赖 biz/alarm 的单例、离线自测能自己决定"这几路算在线"。
 * 没装配就报 false 而不是 true：对端看到 ONLINE 会起播放器，看到 OFFLINE 只显示占位——
 * 少播一路是可见的小问题，报个播不出来的在线是要人现场排查的大问题。
 */
bool channelUp(const HttpApiContext &ctx, int channel);

/// 各章节自己的注册函数（拆开的目的是让自测能只装一节，见各 .cpp 文件头）
void registerAlarmRoutes(PadHttpServer *server, const HttpApiContext &ctx);    ///< §4
void registerFenceRoutes(PadHttpServer *server, const HttpApiContext &ctx);    ///< §5
void registerStreamRoutes(PadHttpServer *server, const HttpApiContext &ctx);   ///< §6
void registerDeviceRoutes(PadHttpServer *server, const HttpApiContext &ctx);   ///< §7

}  // namespace pad

#endif  // PAD_HTTP_API_H
