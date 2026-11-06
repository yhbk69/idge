// ============================================================================
// pad_wire.cpp — 见 pad_wire.h（三条必须选一个的口径都写在那份文件头）
// ============================================================================

#include "pad_wire.h"
#include "alarm_labels.h"

#include "alarm_uplink_engine.h"   // uplink::mediaUrlFor（越界即不给的安全判据）
#include "construction_session.h"  // session::ConstructionInfo::taskIdFromJsonText（taskId 的唯一取值判据）

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonParseError>
#include <QSysInfo>   // machineHostName()：defaultDeviceId() 的兜底来源

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
    // 见文件头第 1 条：rectified 是"已确认/已读"，只有 AlarmDAO::dispose() 写的
    // disposed 才是 v1 的 DISPOSED——把 rectified 折成 DISPOSED 等于把"我看过了"
    // 上报成"我处理过了"，而库里现在几万条 rectified 全是板端点过确认的历史行。
    const QString &s = record.status;
    if (s == QLatin1String("false_alarm")) {
        return QStringLiteral("FALSE_ALARM");
    }
    if (s == QLatin1String("disposed")) {
        return QStringLiteral("DISPOSED");
    }
    // pending 与 rectified 都落这里；§3.10 还规定"其他值按 PENDING 处理"，
    // 所以认不出的内部值同样走这条，不返回空串（对端会把空值当成字段残缺）。
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
    // B2 于 2026-11-04 定案：v1 的 taskId 就是本机施工计划 ID（扫码载荷的 plan.taskId），
    // 库里它已升成一等列（record.taskId），所有过滤都按它等值走。
    // 列没值的两种情况回落去解那段 JSON：迁移前的老行、以及内存里刚造出来还没落库的行。
    if (!record.taskId.isEmpty()) {
        return record.taskId;
    }
    return session::ConstructionInfo::taskIdFromJsonText(record.constructionJson);
}

QString defaultDeviceId()
{
    static const QString cached = [] {
        // 前缀与 §7.1 示例的 "EDGE-05-007" 同族；machine-id 取前 12 位十六进制，
        // 够唯一又能抄进现场记录（完整 32 位没人会在 Pad 屏幕上核对）。
        QFile machineId(QStringLiteral("/etc/machine-id"));
        if (machineId.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString id = QString::fromLatin1(machineId.readAll()).trimmed().toUpper();
            if (!id.isEmpty()) {
                return QStringLiteral("EDGE-") + id.left(12);
            }
        }
        // QSysInfo::machineHostName() 在 Qt 5.12 就返回 QString，不要再包一层 fromLatin1
        return QStringLiteral("EDGE-") + QSysInfo::machineHostName().toUpper();
    }();
    return cached;
}

TypeWire typeOf(const AlarmRecord &record)
{
    // 表体已移到 src/biz/alarm/alarm_labels.cpp：`rule` 现在要落库并在本机详情框显示，
    // 产生方是 biz/alarm，类别→标签这张表不能两处各留一份（改一处忘另一处＝对外口径分叉）。
    const alarm_labels::TypeWire wire = alarm_labels::typeWireOf(record.alarmType, record.className);
    return {wire.code, wire.typeLabel, wire.categoryLabel};
}

namespace {

// 报警片段的可播地址：库里存的是 Annex-B 裸流（.h265/.h264），同名 .mp4 才是能播的那份。
// 两者都要求存在；片段是"报警后 2 秒"才写出来，所以报警当刻通常给不出（返回空串）。
QString clipUrlOf(const AlarmRecord &record, const QString &baseUrl, const QString &rootDir)
{
    if (record.videoPath.isEmpty())
        return QString();
    const QFileInfo clipInfo(record.videoPath);
    const QString mp4 = clipInfo.absolutePath() + QLatin1Char('/')
            + clipInfo.completeBaseName() + QStringLiteral(".mp4");
    if (!QFile::exists(mp4))
        return QString();
    return uplink::mediaUrlFor(baseUrl, rootDir, mp4);
}

}  // namespace

QJsonObject evidenceOf(const AlarmRecord &record, const QString &baseUrl, const QString &rootDir)
{
    // §13.2.1（2026-11-05 改版）：证据两字段**直接放在 `alarm` 顶层**，不再用 `evidence`
    // 对象包裹，也不再有 `snapshotUrls` 数组。本函数返回的就是"要并进 alarm 的那两个键"，
    // 由 alarmJson() 逐键 insert。
    //
    // 这一处改过三轮，别再凭"更好判空"动它：
    //   2026-11-04 本机改成 evidence.evidenceSnapshotUrl（被 11-05 的规约否掉）
    //   11-05 上午按当时规约改成 evidence.snapshotUrls（真机读不到 → 图片恒空）
    //   11-05 下午规约定为顶层 evidenceSnapshotUrl + clipUrl（即下面这版）
    QJsonObject fields;

    const QString snapAbs = QFileInfo(record.imagePath).absoluteFilePath();
    const QString snapUrl = uplink::mediaUrlFor(baseUrl, rootDir, snapAbs);
    // evidenceSnapshotUrl 是**必填**（string）。取不到地址时给空串而不是省略键：
    // 省略 = 必填缺失，严格实现会整条拒收；空串 = 字段在、值无效，Pad 按 §3.5
    // "无图/加载失败显示占位"处理。两种都行的情况下，绝不拼一个对端取不到的地址。
    fields.insert(QStringLiteral("evidenceSnapshotUrl"), snapUrl);

    // 片段"报警后约 2 秒"才落盘，所以首帧这里是 null；到位后按 §3.6 发一条
    // **alarm_update 带 clipUrl**（用户 2026-11-05 指定的流程：先发信息+图片，视频写完再更新）。
    const QString clipUrl = clipUrlOf(record, baseUrl, rootDir);
    fields.insert(QStringLiteral("clipUrl"),
                  clipUrl.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(clipUrl));
    return fields;
}

namespace {

// §13.2.3 的 `dispose` 对象。`alarm` 与 `alarm_update` **共用这一份**：
// 两处各拼一遍的话，别的 Pad 收到的"部分更新"里就会缺照片/工号，而本机详情框里有——
// 同一个报警在两端显示成两种内容。
QJsonObject disposeObjectOf(const AlarmRecord &record, const QString &baseUrl,
                            const QString &rootDir)
{
    QJsonObject dispose;
    dispose.insert(QStringLiteral("measure"), record.disposeResult);
    dispose.insert(QStringLiteral("remark"), record.remark);
    // personId：库里是 dispose_person_id 列（字符串工号）。没给就整个键不写——
    // 写空串会被对端读成"处置人工号是空"，那是另一种假数。
    if (!record.disposePersonId.isEmpty())
        dispose.insert(QStringLiteral("personId"), record.disposePersonId);
    dispose.insert(QStringLiteral("personName"), record.disposeUserName);
    // disposedAt 是"终态时必填"。老行可能终态但没有 dispose_time（迁移前只写了状态），
    // 这时**宁可省略也不填现在**：wireTime("") 会回落成当前时刻，那等于伪造一个处置时间。
    if (!record.disposeTime.isEmpty())
        dispose.insert(QStringLiteral("disposedAt"), wireTime(record.disposeTime));

    // photoUrls：库里那列现在是**JSON 数组文本**（0~5 张），但老行是单张裸路径，
    // 所以两种形状都认：以 '[' 开头就按数组解，否则当成一个元素。
    // 判据是"第一个字符"而不是 try-parse：一条被截断的坏 JSON 会走数组分支然后
    // 解出空表，那等于把"有照片但读不出"报成"没照片"。
    QJsonArray photos;
    const QString stored = record.disposePhoto.trimmed();
    QJsonArray storedPaths;
    if (stored.startsWith(QLatin1Char('['))) {
        const QJsonDocument doc = QJsonDocument::fromJson(stored.toUtf8());
        if (doc.isArray())
            storedPaths = doc.array();
    } else if (!stored.isEmpty()) {
        storedPaths.append(stored);
    }
    for (const QJsonValue &item : storedPaths) {
        const QString path = item.toString();
        if (path.isEmpty())
            continue;
        // 库里那列存的是**相对媒体根**的路径（`dispose/<alarmId>/dispose1.jpg`，
        // POST /api/alarms/{id}/dispose 就是这么写的），所以要按 rootDir 取绝对路径。
        // 早先这里用 QFileInfo(path).absoluteFilePath()，那是按**当前工作目录**展开的
        // ——run.sh 的 CWD 是 build 目录、媒体根是 build/data/alarms，于是相对路径被展开成
        // build/dispose/... ，mediaUrlFor 一看越界就返回空串，症状是"Pad 上处置照片永远是空
        // 数组，本机看却装着有"。绝对路径（老数据/外部工具写的）仍然原样交给 mediaUrlFor 判界。
        const QString abs = QFileInfo(path).isAbsolute()
                                ? path
                                : QDir(rootDir).filePath(path);
        const QString url = uplink::mediaUrlFor(baseUrl, rootDir, abs);
        if (!url.isEmpty())
            photos.append(url);   // 越出媒体根的一律不列（mediaUrlFor 的判据）
    }
    dispose.insert(QStringLiteral("photoUrls"), photos);
    return dispose;
}

}  // namespace

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

    // §13.2.1：证据字段**平铺**在 alarm 顶层（evidenceSnapshotUrl / clipUrl），
    // 没有 evidence 对象。sync_result.alarms[] 用的是同一份快照，所以两处一起就对了。
    const QJsonObject evidenceFields = evidenceOf(record, baseUrl, rootDir);
    for (auto it = evidenceFields.constBegin(); it != evidenceFields.constEnd(); ++it)
        data.insert(it.key(), it.value());

    // §13.2.1 的四个溯源字段：2026-11-06 起在报警产生那一刻一起落库，所以只有新行给得出。
    // 历史行为空时**整个键省略**（§2.6：可选字段可省略，Pad 按未提供处理），
    // 写空串会被读成"这条报警的规则是空字符串"，那是另一种假数。
    if (!record.rule.isEmpty())
        data.insert(QStringLiteral("rule"), record.rule);
    if (!record.modelVersion.isEmpty())
        data.insert(QStringLiteral("modelVersion"), record.modelVersion);
    // fenceVersion 规约是字符串 "V2"；库里存的是那个 2。0 = 迁移前的老行/非围栏报警
    if (record.fenceVersion > 0)
        data.insert(QStringLiteral("fenceVersion"),
                    QStringLiteral("V%1").arg(record.fenceVersion));
    if (!record.regionId.isEmpty())
        data.insert(QStringLiteral("regionId"), record.regionId);

    if (!record.disposeResult.isEmpty() || !record.disposeUserName.isEmpty()
            || !record.disposeTime.isEmpty()) {
        data.insert(QStringLiteral("dispose"), disposeObjectOf(record, baseUrl, rootDir));
    }
    return data;
}

QJsonObject alarmUpdateJson(const AlarmRecord &record, const QString &baseUrl,
                            const QString &rootDir)
{
    QJsonObject data;
    data.insert(QStringLiteral("alarmId"), record.id);
    const QString taskId = taskIdOf(record);
    if (!taskId.isEmpty()) {
        data.insert(QStringLiteral("taskId"), taskId);   // §13.2.2 可选
    }
    // status / readAt / dispose 至少给一个（§13.2.2）。本机三条状态入口各自带齐了自己那部分：
    // 已读只改 read_time，处置/误报同时改 status 与 dispose 各列。
    data.insert(QStringLiteral("status"), wireStatus(record));

    const QString readAt = wireReadAt(record);
    if (!readAt.isEmpty()) {
        data.insert(QStringLiteral("readAt"), readAt);
    }
    if (!record.disposeResult.isEmpty() || !record.disposeUserName.isEmpty()
            || !record.disposeTime.isEmpty()) {
        // 与 alarm 共用同一个 builder：照片/工号在"部分更新"里也不能丢，
        // 否则别的 Pad 只收到状态变化，处置现场照片永远补不上（§13.2.3 的 photoUrls）。
        data.insert(QStringLiteral("dispose"), disposeObjectOf(record, baseUrl, rootDir));
    }
    // 视频落盘后**只发这一条 update**（2026-11-05 指定流程：报警当刻先发信息+图片，
    // 片段写完再用 alarm_update 更新视频显示）。
    // clipUrl 只在能给出地址时才插入，**绝不给 null**：§13.2.2"未出现的字段表示未提供，
    // 不能当作空值清空本地"——发一个 null 会把对端已经拿到的视频清掉。
    // ⚠ 这个字段 §13.2.2 的表格里还没有，是对端要补进规约的一项（本机按现场指令先落）。
    const QString clipUrl = clipUrlOf(record, baseUrl, rootDir);
    if (!clipUrl.isEmpty()) {
        data.insert(QStringLiteral("clipUrl"), clipUrl);
    }
    // §13.2.2 updatedAt 必填且必须反映本次变更，否则对端按旧值合并会把这条更新吞掉。
    data.insert(QStringLiteral("updatedAt"),
                wireTime(record.updateTime.isEmpty() ? record.alarmTime : record.updateTime));
    // syncState：本机没有"再往中心服务器上传"这一步（§8/§11 未装），所以恒 SUBMITTED
    // （= 已接收待上传）。**不报 UPLOADED**——那是假数；等真正接到上传任务再改这一处。
    data.insert(QStringLiteral("syncState"), QStringLiteral("SUBMITTED"));
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
    return envelopeWithCode(type, 0, QStringLiteral("ok"), data);
}

QByteArray pongEnvelope()
{
    // §3.2：服务端 → Pad 的消息**必须带** code/message。ping 的应答虽然是空 data，
    // 也不能例外——少这两个键，字典校验严格的对端会把这条判成不合规连接。
    return envelopeWithCode(QStringLiteral("pong"), 0, QStringLiteral("ok"), QJsonObject());
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
