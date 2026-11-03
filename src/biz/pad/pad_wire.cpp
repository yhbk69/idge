// ============================================================================
// pad_wire.cpp — 见 pad_wire.h（三条必须选一个的口径都写在那份文件头）
// ============================================================================

#include "pad_wire.h"

#include "alarm_uplink_engine.h"   // uplink::mediaUrlFor（越界即不给的安全判据）

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonParseError>

namespace pad {

namespace {

// Qt 5.12 没有 offsetFromUtc()/UTCOffset 对 LocalTime 的可靠实现（offsetFromUtc 是 5.13+），
// 所以自己求：把同一个墙上时间按 UTC 解释一次，两者之差就是本机相对 UTC 的偏移。
// 板端时区 CST+0800 → +28800 秒；夏令时不存在的地区也照样跟着系统时区走，不写死 +08:00。
int offsetSecondsOf(const QDateTime &localDateTime)
{
    if (!localDateTime.isValid()) {
        return 0;
    }
    const QDateTime wallAsUtc(localDateTime.date(), localDateTime.time(), Qt::UTC);
    return static_cast<int>((wallAsUtc.toMSecsSinceEpoch() - localDateTime.toMSecsSinceEpoch())
                            / 1000);
}

QString offsetText(int seconds)
{
    const int sign = seconds < 0 ? -1 : 1;
    const int absSeconds = qAbs(seconds);
    return QString(QStringLiteral("%1%2:%3"))
        .arg(sign < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(absSeconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((absSeconds % 3600) / 60, 2, 10, QLatin1Char('0'));
}

// 内部定宽文本 → 带偏移文本。已经是带偏移/UTC 的原样返回（幂等：老库里的行不会被二次加工）。
QString toWire(const QDateTime &dt)
{
    return dt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss")) + offsetText(offsetSecondsOf(dt));
}

QDateTime parseInternal(const QString &text)
{
    if (text.isEmpty()) {
        return QDateTime();
    }
    QDateTime dt = QDateTime::fromString(text, Qt::ISODate);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(text, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    return dt;
}

} // namespace

QString wireTime(const QString &internalIso)
{
    if (internalIso.contains(QLatin1Char('+')) || internalIso.endsWith(QLatin1Char('Z'))) {
        return internalIso;
    }
    const QDateTime dt = parseInternal(internalIso);
    if (!dt.isValid()) {
        // updatedAt 是对端合并与游标依据，宁可给"现在"也不能给空串
        return wireNow();
    }
    return toWire(dt);
}

QString wireNow()
{
    return toWire(QDateTime::currentDateTime());
}

QString internalTimeFromWire(const QString &wireTimeText)
{
    if (wireTimeText.isEmpty()) {
        return QString();
    }
    // Qt 5.12 的 ISODate 解析认 "…+08:00" 与小数秒，解出来是 OffsetFromUTC 语义
    QDateTime dt = QDateTime::fromString(wireTimeText, Qt::ISODate);
    if (!dt.isValid()) {
        // 退化路径：对端给了个认不出的形状，截前 19 字符按本机墙上时间读。
        // 认不出就当全量（返回空串）比按错误时刻过滤掉一半报警要好。
        dt = QDateTime::fromString(wireTimeText.left(19), Qt::ISODate);
        if (!dt.isValid()) {
            return QString();
        }
        return dt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    }
    if (dt.timeSpec() != Qt::LocalTime) {
        dt = dt.toLocalTime();
    }
    return dt.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
}

QString wireStatus(const AlarmRecord &record)
{
    // 见文件头第 1 条：rectified 是"已确认/已读"，不是"已处置"。
    const QString &s = record.status;
    if (s == QLatin1String("false_alarm")) {
        return QStringLiteral("FALSE_ALARM");
    }
    if (s == QLatin1String("rectified")) {
        return QStringLiteral("PENDING");
    }
    return QStringLiteral("PENDING");
}

QString wireReadAt(const AlarmRecord &record)
{
    if (!record.readTime.isEmpty()) {
        return wireTime(record.readTime);
    }
    // 只有"确认"动作会置 rectified，那时 read_time 还是空 → 用状态变更时间兜底。
    // 未确认未读的行两个字段都空，返回空串由调用方决定省略还是写 null。
    if (record.status == QLatin1String("rectified")) {
        const QString stamp = record.updateTime.isEmpty() ? record.alarmTime : record.updateTime;
        return stamp.isEmpty() ? QString() : wireTime(stamp);
    }
    return QString();
}

QString taskIdOf(const AlarmRecord &record)
{
    // 唯一来源是扫码登录定死的 construction_id（B2 待与甲方确认是否等同 v1 的 taskId）。
    if (record.constructionJson.isEmpty()) {
        return QString();
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(record.constructionJson.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return QString();
    }
    return doc.object().value(QStringLiteral("construction_id")).toString();
}

TypeWire typeOf(const AlarmRecord &record)
{
    // 围栏报警按 alarmType 定（角色决定语义，与检出什么类别无关）。
    if (record.alarmType == QLatin1String("fence_in")) {
        return {QStringLiteral("INTRUSION"), QStringLiteral("区域闯入"), QStringLiteral("区域违规")};
    }
    if (record.alarmType == QLatin1String("fence_out")) {
        // ⚠ v1 的 CROSS（越界）与本机"离岗"是否同一件事，规约没定义（歧义 A9）。
        //   这里按"人离开所有岗区域 = 越出授权范围"取 CROSS，等接口方确认后只改这一行。
        return {QStringLiteral("CROSS"), QStringLiteral("离岗"), QStringLiteral("区域违规")};
    }

    // 检测报警按类别名（labels.txt 行内容，各模型自定，故小写原样比对）。
    static const struct {
        const char *cls;
        const char *code;
        const char *typeLabel;
        const char *categoryLabel;
    } kTable[] = {
        {"un_helmet", "HELMET",  "未戴安全帽",  "PPE违规"},
        {"helmet",    "HELMET",  "未戴安全帽",  "PPE违规"},
        {"vest",      "VEST",    "未穿反光衣",  "PPE违规"},
        {"workWear",  "VEST",    "未穿反光衣",  "PPE违规"},
        {"cigarette", "SMOKING", "吸烟",        "现场隐患"},
        {"smoking",   "SMOKING", "吸烟",        "现场隐患"},
        {"fire",      "FIRE",    "明火",        "现场隐患"},
        {"smoke",     "SMOKE",   "烟雾",        "现场隐患"},
    };
    for (const auto &row : kTable) {
        if (record.className == QLatin1String(row.cls)) {
            return {QString::fromUtf8(row.code), QString::fromUtf8(row.typeLabel),
                    QString::fromUtf8(row.categoryLabel)};
        }
    }

    // 兜底：v1 说"Pad 不做本地类型映射"，所以标签必须由这边给，不能空着。
    // 取不到就把类别名原样抬成大写 code + 同名标签，对端至少能显示"是什么检出的"。
    TypeWire fallback;
    fallback.code = record.className.isEmpty() ? QStringLiteral("UNKNOWN")
                                               : record.className.toUpper();
    fallback.typeLabel = record.className.isEmpty() ? QStringLiteral("未知报警")
                                                    : record.className;
    fallback.categoryLabel = QStringLiteral("其他");
    return fallback;
}

QJsonObject evidenceOf(const AlarmRecord &record, const QString &baseUrl, const QString &rootDir)
{
    QJsonObject evidence;
    QJsonArray snapshots;

    const QString snapAbs = QFileInfo(record.imagePath).absoluteFilePath();
    const QString snapUrl = uplink::mediaUrlFor(baseUrl, rootDir, snapAbs);
    if (!snapUrl.isEmpty()) {
        snapshots.append(snapUrl);   // v1 §3.6：数组只放 1 个元素，Pad 只取第一个
    }
    evidence.insert(QStringLiteral("snapshotUrls"), snapshots);

    // 库里存的是 Annex-B 裸流；同名 .mp4 才是能播的那个（见 alarm_manager.h 的 attachVideoClip）。
    // 文件还没落盘就不写这个键——"预告了但取不到"比对端更坏。
    if (!record.videoPath.isEmpty()) {
        const QFileInfo clipInfo(record.videoPath);
        const QString mp4 = clipInfo.absolutePath() + QLatin1Char('/')
                + clipInfo.completeBaseName() + QStringLiteral(".mp4");
        if (QFile::exists(mp4)) {
            const QString clipUrl = uplink::mediaUrlFor(baseUrl, rootDir, mp4);
            if (!clipUrl.isEmpty()) {
                evidence.insert(QStringLiteral("clipUrl"), clipUrl);
            }
        }
    }
    return evidence;
}

QJsonObject alarmJson(const AlarmRecord &record, const QString &baseUrl, const QString &rootDir)
{
    const TypeWire type = typeOf(record);

    QJsonObject data;
    data.insert(QStringLiteral("alarmId"), record.id);
    data.insert(QStringLiteral("taskId"), taskIdOf(record));
    data.insert(QStringLiteral("level"), record.alarmLevel);
    data.insert(QStringLiteral("type"), type.code);
    data.insert(QStringLiteral("typeLabel"), type.typeLabel);
    data.insert(QStringLiteral("categoryLabel"), type.categoryLabel);
    data.insert(QStringLiteral("status"), wireStatus(record));
    data.insert(QStringLiteral("occurredAt"), wireTime(record.alarmTime));
    data.insert(QStringLiteral("updatedAt"),
                wireTime(record.updateTime.isEmpty() ? record.alarmTime : record.updateTime));

    const QString readAt = wireReadAt(record);
    data.insert(QStringLiteral("readAt"), readAt.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                          : QJsonValue(readAt));
    data.insert(QStringLiteral("confidence"), static_cast<double>(record.confidence));
    data.insert(QStringLiteral("evidence"), evidenceOf(record, baseUrl, rootDir));

    // 可选键：本机现在给不出的（rule/fenceVersion/regionId/modelVersion）整个省略，
    // 不写空串——v1 §2.6 规定"可选字段可以省略或 null，Pad 按未提供处理"。
    if (!record.disposeResult.isEmpty() || !record.disposeUserName.isEmpty()
            || !record.disposeTime.isEmpty()) {
        QJsonObject dispose;
        dispose.insert(QStringLiteral("measure"), record.disposeResult);
        dispose.insert(QStringLiteral("remark"), record.remark);
        dispose.insert(QStringLiteral("personName"), record.disposeUserName);
        dispose.insert(QStringLiteral("disposedAt"), wireTime(record.disposeTime));
        QJsonArray photos;
        if (!record.disposePhoto.isEmpty()) {
            const QString url = uplink::mediaUrlFor(
                baseUrl, rootDir, QFileInfo(record.disposePhoto).absoluteFilePath());
            if (!url.isEmpty()) {
                photos.append(url);
            }
        }
        dispose.insert(QStringLiteral("photoUrls"), photos);
        data.insert(QStringLiteral("dispose"), dispose);
    }
    return data;
}

QJsonObject alarmUpdateJson(const AlarmRecord &record)
{
    QJsonObject data;
    data.insert(QStringLiteral("alarmId"), record.id);
    const QString taskId = taskIdOf(record);
    if (!taskId.isEmpty()) {
        data.insert(QStringLiteral("taskId"), taskId);   // §13.2.3 可选
    }
    data.insert(QStringLiteral("status"), wireStatus(record));

    const QString readAt = wireReadAt(record);
    if (!readAt.isEmpty()) {
        data.insert(QStringLiteral("readAt"), readAt);
    }
    if (!record.disposeResult.isEmpty()) {
        QJsonObject dispose;
        dispose.insert(QStringLiteral("measure"), record.disposeResult);
        dispose.insert(QStringLiteral("remark"), record.remark);
        dispose.insert(QStringLiteral("personName"), record.disposeUserName);
        dispose.insert(QStringLiteral("disposedAt"), wireTime(record.disposeTime));
        data.insert(QStringLiteral("dispose"), dispose);
    }
    // §3.7：updatedAt 必填且必须反映本次变更，否则对端按旧值合并会把这条更新吞掉。
    data.insert(QStringLiteral("updatedAt"),
                wireTime(record.updateTime.isEmpty() ? record.alarmTime : record.updateTime));
    return data;
}

QByteArray envelopeWithCode(const QString &type, int code, const QString &message,
                            const QJsonObject &data)
{
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), type);
    msg.insert(QStringLiteral("ts"), wireNow());
    msg.insert(QStringLiteral("code"), code);
    msg.insert(QStringLiteral("message"), message);
    if (!data.isEmpty()) {
        msg.insert(QStringLiteral("data"), data);
    }
    return QJsonDocument(msg).toJson(QJsonDocument::Compact);
}

QByteArray envelope(const QString &type, const QJsonObject &data)
{
    if (type == QLatin1String("pong")) {
        return pongEnvelope();
    }
    return envelopeWithCode(type, 0, QStringLiteral("ok"), data);
}

QByteArray pongEnvelope()
{
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("pong"));
    msg.insert(QStringLiteral("ts"), wireNow());
    return QJsonDocument(msg).toJson(QJsonDocument::Compact);
}

QByteArray errorEnvelope(int code, const QString &message, const QString &detail)
{
    QJsonObject data;
    if (!detail.isEmpty()) {
        data.insert(QStringLiteral("detail"), detail);
    }
    QJsonObject msg;
    msg.insert(QStringLiteral("type"), QStringLiteral("error"));
    msg.insert(QStringLiteral("ts"), wireNow());
    msg.insert(QStringLiteral("code"), code);
    msg.insert(QStringLiteral("message"), message);
    if (!data.isEmpty()) {
        msg.insert(QStringLiteral("data"), data);
    }
    return QJsonDocument(msg).toJson(QJsonDocument::Compact);
}

} // namespace pad
