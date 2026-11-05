// ============================================================================
// pad_http_api.cpp — §4~§7 路由的整体装配
// ============================================================================
// 本文件只做一件事：按章节把路由装到同一台 PadHttpServer 上。
// 装两次不会翻倍——addRoute() 对同 (method, pattern) 是覆盖语义（见 pad_http_server.cpp），
// 而这里每次都先 clearRoutes()，所以「保存并应用」反复装配出来永远是一张干净的表。
// ============================================================================
#include "pad_http_api.h"

#include "ConfigManager.h"
#include "rtsp_live_service.h"   // 带检测框的 RTSP 直播源（§6.1 liveUrl 优先指它）

namespace pad {

namespace {

/// 从 URL 里去掉 `user:pass@` 那一段。只处理 rtsp/http(s) 这几种带 authority 的写法；
/// 找不到 '@' 就原样返回。
/// 为什么默认要剥：视频源口令是**这台摄像头**的凭据，而 §6.1 的 liveUrl 会被原样显示、
/// 缓存、也可能进对端日志。局域网零鉴权是规约 §2.3 认了的口径，那是"这台设备能收报警"，
/// 不等于"顺手把每台摄像头的口令发出去"。要保留就显式配 expose_stream_credentials=true。
QString stripCredentials(const QString &url)
{
    const int schemeEnd = url.indexOf(QLatin1String("://"));
    if (schemeEnd < 0)
        return url;
    const int authorityEnd = url.indexOf(QLatin1Char('/'), schemeEnd + 3);
    const int pathAt = authorityEnd < 0 ? url.size() : authorityEnd;
    const int at = url.lastIndexOf(QLatin1Char('@'), pathAt - 1);
    if (at < 0 || at < schemeEnd)
        return url;   // authority 里没有凭据段（或写法奇怪），不动
    // 保留 '@' **之后**的全部内容——那才是 host:port/path。
    // ⚠ 这里踩过一次实测的坑：早先写成从 pathAt 起接，于是
    //   rtsp://admin:pw@192.168.124.36:554/live 被剥成 rtsp:///live（host 一起掉了），
    //   地址看着像那么回事但谁也播不了。
    return url.left(schemeEnd + 3) + url.mid(at + 1);
}

/// 地址属于哪种播放协议（§13.6.1 的 liveProtocol 取值）。
/// 返回空 = 这个地址**不是**局域网另一台设备能取的东西（本地文件、/dev/videoNN、
/// GB28181 的编解码通道 id 等），调用方据此把这一路报成 OFFLINE + 空 liveUrl。
QString detectProtocol(const QString &url)
{
    if (url.startsWith(QLatin1String("rtsp://"), Qt::CaseInsensitive))
        return QStringLiteral("RTSP");
    if (url.startsWith(QLatin1String("rtmp://"), Qt::CaseInsensitive))
        return QStringLiteral("RTMP");
    if (url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        || url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        // .m3u8 才算 HLS：其余 http 地址对端拿到也不知道按什么协议播，
        // 报 HTTP 是让 Pad 明确走"协议不支持"那条显示分支，而不是黑屏。
        return url.contains(QLatin1String(".m3u8"), Qt::CaseInsensitive)
            ? QStringLiteral("HLS") : QStringLiteral("HTTP");
    }
    return QString();
}

}  // namespace

ChannelIdentity channelIdentity(int channel, bool exposeCredentials)
{
    ChannelIdentity id;
    const ConfigManager &cfg = ConfigManager::instance();
    if (channel < 0 || channel >= cfg.channelCount())
        return id;
    const VideoChannelCfg ch = cfg.channelCfg(channel);
    const VideoSourceCfg src = cfg.resolveChannelSource(channel);

    // streamId 按**通道号**派生而不是用注册表 id：两路通道可以引用同一条流
    // （现场就这么用过：一路取主码流、一路取子码流是两条 source，
    //  但也存在"同一 id 被两个通道引用"的配置），用 id 当 streamId 就会撞车。
    // CH1..CH4 与 alarms.channel 的 +1 口径也对得上，排障时不用换一套编号。
    id.streamId = QStringLiteral("CH%1").arg(channel + 1);
    // cameraId 也按**通道号**走，不用注册表 id。
    // 为什么不是 src.id（这曾是本机第一版的做法，被实测否掉了）：
    //   现场那份 config.json 里 CH1/CH2/CH4 三路引用的是**同一条** `file-demo`，
    //   于是 §6 的 streams[] 出现三个相同 cameraId，而 §5.1 的唯一键是 (taskId, cameraId) ——
    //   Pad 在第二路上画的围栏会与第一路撞成同一条围栏，谁的 regions 后写谁赢。
    //   从对端的角度看，"这一路画面"才是它能画围栏、能起播放器的单位，
    //   所以 cameraId 与 streamId 同值；注册表 id 只是本机内部"这路取哪个地址"的实现细节，
    //   规约里没有对应的键，就不塞进去了。
    id.cameraId = id.streamId;
    id.name = ch.note.isEmpty()
        ? (src.note.isEmpty() ? QStringLiteral("通道%1").arg(channel + 1) : src.note)
        : ch.note;
    id.enabled = ch.enabled;

    const QString raw = src.url.trimmed();
    const QString protocol = detectProtocol(raw);

    // 优先返回"带检测框"的 RTSP 直播地址：本机把画好框的画面经 rkmpp 硬编码后
    // 由 ZLM 分发，Pad 拉到的就是有框的实时画面（见 RtspLiveService）。
    // 前提三道全满足才给：通道启用 + RTSP 服务在跑 + 该路编码器真的注册了源。
    // 编码器 init 失败（无 rkmpp）会导致源不存在，hasSource 返回 false → 走原始地址回落。
    if (id.enabled) {
        RtspLiveService &rtsp = RtspLiveService::instance();
        if (rtsp.running() && rtsp.hasSource(id.streamId)) {
            const QString base = rtsp.baseUrl();
            if (!base.isEmpty()) {
                id.playable = true;
                id.liveProtocol = QStringLiteral("RTSP");
                id.liveUrl = QStringLiteral("%1/live/%2").arg(base, id.streamId);
                return id;
            }
        }
    }

    // 回落：原始视频源地址（无标注）。file://、/dev/videoN、GB28181 通道 id 等
    // "局域网另一台设备取不到"的源 detectProtocol 返回空，下面 playable 仍为 false。
    if (!protocol.isEmpty() && id.enabled && !raw.isEmpty()) {
        id.playable = true;
        id.liveProtocol = protocol;
        id.liveUrl = exposeCredentials ? raw : stripCredentials(raw);
    }
    return id;
}

bool channelUp(const HttpApiContext &ctx, int channel)
{
    if (!ctx.channelOnline)
        return false;
    return ctx.channelOnline(channel);
}

void registerPadHttpRoutes(PadHttpServer *server, const HttpApiContext &ctx)
{
    if (!server)
        return;
    server->clearRoutes();
    registerAlarmRoutes(server, ctx);     // §4
    registerFenceRoutes(server, ctx);     // §5
    registerStreamRoutes(server, ctx);    // §6
    registerDeviceRoutes(server, ctx);    // §7
}

}  // namespace pad
