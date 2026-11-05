// ============================================================================
// pad_http_streams.cpp — v1 §6 实时视频（GET /api/streams、GET /api/streams/{id}/status）
// ============================================================================
//
// 这一节本机是**如实转述配置**，不是"起流"：
//   §6.1 明写"无 stream_start / stream_stop，流由边缘设备持续产出，Pad 只在页面打开播放器"，
//   而本机的取流本来就跟随 video.channels 常驻（解码器在 frmMain 装配时就起来了），
//   所以这里没有任何"按需拉起"的分支，只有"这一路现在配的是什么、现在有没有帧"。
//
// 三条不给/给不了的口径（都是"宁可少给也不给假的"）：
//   1) **playbackUrl / snapshotUrl 整条省略**。本机没有按时序录的回放产物，
//      报警片段是"报警前后那一小段"、按报警 id 组织的，不是某一路的录像时间线；
//      §7.2 的 capabilities.stream.playback 因此报 false，两处必须一致。
//   2) **liveUrl 只给局域网另一台设备真能取的地址**。本地文件（file:///…）、
//      /dev/videoNN、GB28181 的编解码通道 id 都不是地址，给出去 Pad 起播放器就黑屏，
//      所以这种条目报 status=ERROR + liveUrl 空（口径与判据见 pad_http_api.cpp 的
//      detectProtocol/channelIdentity，与 §7.1 的 cameras[] 同一份实现）。
//   3) **视频源口令默认剥掉**（HttpApiContext::expose_stream_credentials，默认 false）。
//      rtsp://user:pass@host 会被原样显示、缓存、可能进对端日志，
//      把每台摄像头的口令整份发给局域网是另一回事，不在"零鉴权收报警"那句的范围内。
//      剥完的对端多半播不起来——那是**可见的失败**，比口令出门好。
//
// taskId：§13.6.1 把它列为**必填**，但本机没有"哪一路属于哪个施工计划"这层绑定
// （报警才有 task_id，通道没有）。所以这里的行为是：要求带上、原样回显到 data.taskId，
// **不按它过滤**。为什么不是"没带就 404"或"随便回个空"：
//   Pad 进视频页就是按当前会话的 taskId 拉的，回显一致它才敢缓存；
//   而按一个不存在的绑定去过滤会返回空数组，看起来像"这台设备没接摄像头"。
// ============================================================================
#include "pad_http_api.h"

#include "ConfigManager.h"
#include "pad_http_server.h"

#include <QJsonArray>
#include <QJsonObject>

namespace pad {
namespace {

/// 一条流对象（§13.6.1）。给不出的字段整个键不写（§2.6"可选字段可以省略或 null"）。
QJsonObject streamJson(int channel, const HttpApiContext &ctx)
{
    const ChannelIdentity identity = channelIdentity(channel, ctx.exposeStreamCredentials);
    QJsonObject obj;
    obj.insert(QStringLiteral("streamId"), identity.streamId);
    obj.insert(QStringLiteral("name"), identity.name);
    obj.insert(QStringLiteral("cameraId"), identity.cameraId);

    const bool up = channelUp(ctx, channel);
    QString status;
    if (!identity.enabled)
        status = QStringLiteral("OFFLINE");          // 配置里这一路就是关的
    else if (identity.playable)
        status = up ? QStringLiteral("ONLINE") : QStringLiteral("OFFLINE");
    else
        status = QStringLiteral("ERROR");            // 有地址，但对端取不到（本机源/GB28181）

    if (identity.playable) {
        obj.insert(QStringLiteral("liveUrl"), identity.liveUrl);
        obj.insert(QStringLiteral("liveProtocol"), identity.liveProtocol);
    } else {
        // 规约允许 OFFLINE/ERROR 时 liveUrl 为空或 null。这里写**空串而不是省略**：
        // Pad 的播放决策顺序是"状态→地址→协议"，缺键与空串都落到"无实时地址"那条显示，
        // 但空串在对端日志里一眼能看出是"本机没给"，缺键像"本机忘了给"。
        obj.insert(QStringLiteral("liveUrl"), QString());
    }
    obj.insert(QStringLiteral("status"), status);
    // expiresAt：本机地址不过期（不是那种带签名的临时播放地址），按规约给 null
    obj.insert(QStringLiteral("expiresAt"), QJsonValue(QJsonValue::Null));
    return obj;
}

/// streamId → 通道号。判据与 channelIdentity() 的派生方式逐字同一套（CH1..CH4），
/// 所以这里不做任何"模糊匹配"：对端拿着一个本机没发过的 id 来问，就是不存在。
int channelFromStreamId(const QString &streamId, int channelCount)
{
    for (int ch = 0; ch < channelCount; ++ch) {
        if (streamId.compare(QStringLiteral("CH%1").arg(ch + 1), Qt::CaseInsensitive) == 0)
            return ch;
    }
    return -1;
}

}  // namespace

void registerStreamRoutes(PadHttpServer *server, const HttpApiContext &ctx)
{
    if (!server)
        return;

    // ── §6.1 GET /api/streams?taskId=... ───────────────────────────────
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/streams"),
                     [ctx](const padhttp::Request &request) -> padhttp::Reply {
                         const QString taskId = request.queryValue(QStringLiteral("taskId"));
                         if (taskId.isEmpty()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少查询参数 taskId"),
                                 QStringLiteral("GET /api/streams?taskId=<施工计划 ID>"));
                         }
                         const ConfigManager &cfg = ConfigManager::instance();
                         QJsonArray streams;
                         for (int ch = 0; ch < cfg.channelCount(); ++ch)
                             streams.append(streamJson(ch, ctx));
                         QJsonObject data;
                         data.insert(QStringLiteral("taskId"), taskId);   // 原样回显，见文件头
                         data.insert(QStringLiteral("streams"), streams); // 恒为数组，空也是 []
                         return padhttp::replyOk(data);
                     });

    // ── §6.2 GET /api/streams/{streamId}/status ────────────────────────
    server->addRoute(QStringLiteral("GET"),
                     QStringLiteral("/api/streams/{streamId}/status"),
                     [ctx](const padhttp::Request &request) -> padhttp::Reply {
                         const QString streamId = request.pathParam(QStringLiteral("streamId"));
                         const ConfigManager &cfg = ConfigManager::instance();
                         const int channel = channelFromStreamId(streamId, cfg.channelCount());
                         if (channel < 0) {
                             return padhttp::replyNotFound(
                                 QString::fromUtf8("本机没有这条视频流"),
                                 QStringLiteral("streamId=%1").arg(streamId));
                         }
                         const QJsonObject full = streamJson(channel, ctx);
                         QJsonObject data;
                         data.insert(QStringLiteral("streamId"), full.value(QStringLiteral("streamId")));
                         data.insert(QStringLiteral("status"), full.value(QStringLiteral("status")));
                         // error：规约留的是 string?。本机没有"为什么不在"的细分原因可报
                         // （在线表只有 true/false），所以给 null而不是编一句话。
                         data.insert(QStringLiteral("error"), QJsonValue(QJsonValue::Null));
                         return padhttp::replyOk(data);
                     });
}

}  // namespace pad
