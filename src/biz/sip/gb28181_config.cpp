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
 *   - serverHost 空、或一条 deviceCode 完整的流都没有 → 返回 false，
 *     main() 据此不启动 manager（这就是"没配国标就不启动"的正常路径）。
 *   - 端口只接受 1~65535 的整数，出现小数或越界直接失败而不是取整。
 *
 * 已知假开关（写文档时别当成可配项）：
 *   - platform.transport：非 udp 只会在 manager 里打一条告警，SIP 永远走 UDP。
 *   - stream 的 width/height/bitrate/fps 只在重编码路径生效，源本身是 H.264 时
 *     走 mp4toannexb 透传，这几个值一个都不看。
 *   - stream.loop 只对 type=mp4 有意义。
 * 三个 stringValue/intValue/boolValue 只在"键存在且类型对"时取，否则落默认值——
 * 所以字符串键写成数字、数字键写成字符串都会被静默忽略。
 */
#include "gb28181_config.h"

#include <QAbstractSocket>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>

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
    for (const QJsonValue &value : streamArray) {
        const QJsonObject s = value.toObject();
        if (s.isEmpty()) continue;
        Gb28181StreamConfig stream;
        stream.type = stringValue(s, "type", QStringLiteral("camera")).toLower();
        stream.source = stringValue(s, "source");
        stream.deviceCode = stringValue(s, "deviceCode");
        stream.channelCode = stringValue(s, "channelCode", stream.deviceCode);
        stream.serverCode = stringValue(s, "serverCode", result.platform.serverCode);
        stream.deviceName = stringValue(s, "deviceName", stream.channelCode);
        stream.serialNumber = stringValue(s, "serialNumber");
        stream.password = stringValue(s, "password", result.platform.password);
        stream.ssrc = stringValue(s, "ssrc");
        stream.transport = stringValue(s, "transport", result.platform.transport).toLower();
        stream.cameraIndex = intValue(s, "cameraIndex", -1);
        stream.width = intValue(s, "width", 1920);
        stream.height = intValue(s, "height", 1080);
        stream.fps = intValue(s, "fps", 25);
        stream.bitrate = intValue(s, "bitrate", 2000000);
        stream.enabled = boolValue(s, "enabled", true);
        stream.loop = boolValue(s, "loop", true);
        if (stream.type == QStringLiteral("rtsp") || stream.type == QStringLiteral("rtmp") ||
            stream.type == QStringLiteral("mp4")) {
            if (stream.source.isEmpty()) continue;
        }
        if (stream.type == QStringLiteral("camera") && stream.source.isEmpty() && stream.cameraIndex < 0)
            stream.cameraIndex = 0;
        if (stream.deviceCode.isEmpty()) continue;
        result.streams.push_back(stream);
    }

    if (result.platform.serverHost.isEmpty() || result.streams.isEmpty()) {
        if (error) *error = QStringLiteral("gb28181 platform or streams is incomplete in %1").arg(path);
        return false;
    }
    *out = result;
    return true;
}
