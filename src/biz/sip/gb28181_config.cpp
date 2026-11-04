/**
 * @file gb28181_config.cpp
 * @brief GB28181 配置的 JSON 载入与校验
 *
 * 布局兼容两种写法（cpp:59-61、90-93）：
 *   新式  { "gb28181": { "platform": {...}, "streams": [...] } }
 *   兼容  { "platform": {...}, "streams": [...] }        // 顶层，老 demo 的配置
 * 先取 gb28181.platform，段不存在才退回顶层 platform；streams 的优先级反过来——
 * 顶层 streams 非空就用它，空了才看 gb28181.streams。两者混写时以顶层流列表为准。
 *
 * 校验是"缺关键项就当没配"的策略，不是尽量容错：
 *   - localHost 必须是 IPv4 字面量；advertiseHost 还额外不许是 0.0.0.0，
 *     因为要把它写进 Via/Contact，通配地址平台回不了话。
 *   - serverHost 空、或一条可用的流都没有 → 返回 false，
 *     main() 据此不启动 manager（这就是"没配国标就不启动"的正常路径）。
 *   - 一条流"可用"= 有 deviceCode + 有 source_id 且该 id 在**本文档的** video.sources[] 里查得到。
 *     取流字段（type/source/width/height/fps/loop/cameraIndex）**不再在本段解析**：
 *     它们属于流，属于 `video.sources[]`，与 video.channels 引用的是同一份。
 *   - 端口只接受 1~65535 的整数，出现小数或越界直接失败而不是取整。
 *
 * 已知假开关（写文档时别当成可配项）：
 *   - platform.transport：非 udp 只会在 manager 里打一条告警，SIP 永远走 UDP。
 *   - stream 的 bitrate（以及注册表没给宽高时那对 1920x1080@25 默认值）只在重编码路径
 *     生效，源本身是 H.264 时走 mp4toannexb 透传，这几个值一个都不看。
 *   - 注册表条目的 timeoutMs / reconnectMs **对国标路径不生效**：这条链路的开/停由 SIP
 *     会话控制（INVITE 起、BYE 停），没有"打不开就按间隔重试"的那层外环，硬接会变成
 *     会话早已结束还在后台重连。见 gb28181_media.cpp 文件头。
 * 三个 stringValue/intValue/boolValue 只在"键存在且类型对"时取，否则落默认值——
 * 所以字符串键写成数字、数字键写成字符串都会被静默忽略。
 */
#include "gb28181_config.h"

#include <QAbstractSocket>
#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QStringList>

namespace {
QString stringValue(const QJsonObject &o, const char *key, const QString &fallback = {}) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : fallback;
}

int intValue(const QJsonObject &o, const char *key, int fallback) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt(fallback) : fallback;
}

bool boolValue(const QJsonObject &o, const char *key, bool fallback) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool(fallback) : fallback;
}

bool readPort(const QJsonObject &object, const char *key, int fallback, int *port, QString *error) {
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined()) {
        *port = fallback;
        return true;
    }
    const int parsed = value.toInt(-1);
    if (!value.isDouble() || parsed < 1 || parsed > 65535 || value.toDouble() != parsed) {
        if (error) *error = QStringLiteral("gb28181.platform.%1 must be an integer from 1 to 65535")
            .arg(QLatin1String(key));
        return false;
    }
    *port = parsed;
    return true;
}
}

bool Gb28181Config::load(const QString &path, Gb28181Config *out, QString *error) {
    if (!out) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("invalid JSON in %1: %2").arg(path, parseError.errorString());
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonObject gb = root.value(QStringLiteral("gb28181")).toObject();
    const QJsonObject p = gb.isEmpty()
        ? root.value(QStringLiteral("platform")).toObject()
        : gb.value(QStringLiteral("platform")).toObject();
    Gb28181Config result;
    result.configPath = path;
    result.platform.localHost = stringValue(p, "localHost", QStringLiteral("0.0.0.0"));
    if (!readPort(p, "localSipPort", 5060, &result.platform.localSipPort, error)) return false;
    result.platform.advertiseHost = stringValue(p, "advertiseHost", result.platform.localHost);
    if (!readPort(p, "advertiseSipPort", result.platform.localSipPort,
                  &result.platform.advertiseSipPort, error)) return false;
    const QHostAddress bindAddress(result.platform.localHost);
    const QHostAddress advertisedAddress(result.platform.advertiseHost);
    if (bindAddress.protocol() != QAbstractSocket::IPv4Protocol) {
        if (error) *error = QStringLiteral("gb28181.platform.localHost must be an IPv4 address");
        return false;
    }
    if (advertisedAddress.protocol() != QAbstractSocket::IPv4Protocol ||
        advertisedAddress.toIPv4Address() == 0) {
        if (error) *error = QStringLiteral("gb28181.platform.advertiseHost must be a non-wildcard IPv4 address; set it explicitly when localHost is 0.0.0.0");
        return false;
    }
    result.platform.serverHost = stringValue(p, "serverHost");
    if (!readPort(p, "serverPort", 5060, &result.platform.serverPort, error)) return false;
    result.platform.serverCode = stringValue(p, "serverCode");
    result.platform.serverDomain = stringValue(p, "serverDomain");
    result.platform.password = stringValue(p, "password");
    result.platform.transport = stringValue(p, "transport", QStringLiteral("udp")).toLower();
    result.platform.registerExpires = intValue(p, "registerExpires", 3600);
    result.platform.heartbeatInterval = intValue(p, "heartbeatInterval", 30);
    result.platform.userAgent = stringValue(p, "userAgent", QStringLiteral("idge/GB28181"));

    QJsonArray streamArray = root.value(QStringLiteral("streams")).toArray();
    if (streamArray.isEmpty()) {
        streamArray = gb.value(QStringLiteral("streams")).toArray();
    }
    QStringList dropped;
    int index = 0;
    for (const QJsonValue &value : streamArray) {
        ++index;
        const QJsonObject s = value.toObject();
        if (s.isEmpty()) continue;
        Gb28181StreamConfig stream;
        stream.sourceId = stringValue(s, "source_id").trimmed();
        stream.deviceCode = stringValue(s, "deviceCode");
        stream.channelCode = stringValue(s, "channelCode", stream.deviceCode);
        stream.serverCode = stringValue(s, "serverCode", result.platform.serverCode);
        stream.deviceName = stringValue(s, "deviceName", stream.channelCode);
        stream.serialNumber = stringValue(s, "serialNumber");
        stream.password = stringValue(s, "password", result.platform.password);
        stream.ssrc = stringValue(s, "ssrc");
        stream.transport = stringValue(s, "transport", result.platform.transport).toLower();
        stream.bitrate = intValue(s, "bitrate", 2000000);
        stream.enabled = boolValue(s, "enabled", true);
        if (stream.deviceCode.isEmpty()) {
            dropped << QStringLiteral("第 %1 条流没有 deviceCode（国标通道编码，平台按它点流）").arg(index);
            continue;
        }
        if (stream.sourceId.isEmpty()) {
            dropped << QStringLiteral("第 %1 条流（deviceCode=%2）没有 source_id——取流地址现在只在 "
                                     "video.sources[] 里，本段不再写 type/source/width/height/fps/loop")
                           .arg(QString::number(index), stream.deviceCode);
            continue;
        }
        // 悬空判据查**这份文档**，不查 ConfigManager 单例：main() 里这次 load() 跑在
        // frmMain 的 ConfigManager::load() 之前，那时单例还是空的，查它会把每条流都判成悬空。
        if (ConfigManager::videoSourceInRoot(root, stream.sourceId).url.isEmpty()) {
            dropped << QStringLiteral("第 %1 条流（deviceCode=%2）的 source_id=%3 在 video.sources[] 里不存在")
                           .arg(index).arg(stream.deviceCode, stream.sourceId);
            continue;
        }
        result.streams.push_back(stream);
    }
    // 逐条打出来：整段"没配国标"和"配了但写错"必须分得开，否则现场只看到"GB28181 没起来"，
    // 而真实原因可能是 4 条流里 3 条的 source_id 拼错。
    for (const QString &why : qAsConst(dropped)) {
        qWarning() << "GB28181 stream dropped:" << why;
    }

    if (result.platform.serverHost.isEmpty() || result.streams.isEmpty()) {
        if (error) {
            *error = dropped.isEmpty()
                ? QStringLiteral("gb28181 platform or streams is incomplete in %1").arg(path)
                : QStringLiteral("gb28181 has no usable stream in %1（%2 条被丢弃，原因见日志）")
                      .arg(path).arg(dropped.size());
        }
        return false;
    }
    *out = result;
    return true;
}

VideoSourceCfg Gb28181StreamConfig::takeSource() const
{
    // 现取不缓存：设置页改完 video.sources[] 的某个 url，下一次 INVITE 就按新地址开流，
    // 不用重启进程（load() 那份快照只带身份字段，本来就不含取流信息）。
    VideoSourceCfg s = ConfigManager::instance().videoSource(sourceId);
    if (s.url.isEmpty()) {
        // 到这里只剩两种成因：进程跑起来之后注册表被删/被改成悬空，或 id 大小写不一致。
        // 返回空 cfg 让媒体层报错，而不是"随便挑一条流推给平台"——那种兜底会把
        // "平台点 3 号通道看到 1 号画面"这种问题变成常态。
        return VideoSourceCfg();
    }
    // 国标侧必须把类型判出来（媒体层靠它选 v4l2 参数、rtsp tcp、mp4 循环），
    // 而注册表允许 type 留空（解码器按 url 前缀嗅探）。这里补成同一个嗅探规则：
    // 判据与 FFmpegVideoDecoder 的 isCaptureNode()/前缀判定一致，改那边要改这里。
    if (s.type.isEmpty()) {
        if (s.url.startsWith(QLatin1String("/dev/video")))
            s.type = QStringLiteral("camera");
        else if (s.url.startsWith(QLatin1String("rtsp://")))
            s.type = QStringLiteral("rtsp");
        else if (s.url.startsWith(QLatin1String("rtmp://")))
            s.type = QStringLiteral("rtmp");
        else
            s.type = QStringLiteral("file");
    }
    return s;
}
