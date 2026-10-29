// ============================================================================
// alarm_uplink_engine.cpp — 报警上行的报文组装与待发队列
// ----------------------------------------------------------------------------
// 这个文件只做"纯逻辑"：不碰 socket、不读 ConfigManager、不发 Qt 信号。
//   * 报文形状：外层信封 type/message_id/ts/data；报警里的媒体位置是"预告"
//     （data.snapshot / data.clip，planned=true），真实字节数与 sha256 只在 media_begin 给
//   * 分块：一次 sendBinaryMessage = 一个不拆分的 WS 数据帧（Qt 侧没有分片循环），
//     所以块大小由 chunk_size 决定，planChunks 是唯一的换算处
//   * 合流：同一批报警（共享抓拍图路径）复用同一个 media_id，slot 表按
//     kind + 批次键索引，alarm_ids 回指同批
//   * 队列：有界 deque，满了丢最旧（与 SnapWriter/ClipWriter 同一口径）；
//     发送动作全部集中在 pumpOut()，由调用方的定时器/重连信号驱动
// 依赖 Qt5::Core 而已，tests/test_alarm_uplink.cpp 用 FakeTransport 就能跑。
// ============================================================================

#include "alarm_uplink_engine.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QUuid>

namespace uplink {

QString newMediaId(const char *prefix)
{
    // 36 位带连字符的 UUID → 去掉分隔符取前 16 位：够唯一，又不至于把日志行撑爆
    QString hex = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hex.remove(QLatin1Char('-'));
    return QString::fromLatin1(prefix) + hex.left(16);
}

ChunkPlan planChunks(qint64 totalBytes, int chunkSize)
{
    ChunkPlan plan;
    plan.totalBytes = totalBytes;
    // 下限取 1：chunkSize=0 会除零。配置层已经 qBound 过，这里再兜一次保证纯函数不炸
    plan.chunkSize = qMax(1, chunkSize);
    if (totalBytes <= 0) {
        return plan;   // 空文件 = 0 块，调用方按"没有媒体可发"处理
    }
    plan.totalChunks = static_cast<int>((totalBytes + plan.chunkSize - 1) / plan.chunkSize);
    // 末块取余数（整除时恰好等于 chunkSize），所以一定落在 [1, chunkSize]
    plan.lastChunkBytes = static_cast<int>(totalBytes - (plan.totalChunks - 1LL) * plan.chunkSize);
    return plan;
}

qint64 backoffMs(int attempt, int baseMs)
{
    // attempt 从 1 起：base×2、×4、×8……到 kBackoffCapMs 封顶，指数不再往上翻
    if (attempt <= 0) {
        return 0;
    }
    qint64 delay = baseMs;
    for (int i = 1; i < attempt && delay < kBackoffCapMs; ++i) {
        delay *= 2;
    }
    return qMin(delay, kBackoffCapMs);
}

QString mediaUrlFor(const QString &baseUrl, const QString &rootDir, const QString &absPath)
{
    if (baseUrl.isEmpty() || rootDir.isEmpty() || absPath.isEmpty()) {
        return QString();
    }

    // 越界一律不给，不"尽力拼一个看看"：服务是零鉴权的，root 之外的路径一旦拼出来
    // 就等于把本机任意可读文件公开出去。relativeFilePath 会把 `..` 折成 ../ 前缀，正好当判据。
    QString rel = QDir(rootDir).relativeFilePath(absPath);
    if (rel.isEmpty() || rel == QLatin1String(".") || rel == QLatin1String("..")
            || rel.startsWith(QStringLiteral("../")) || rel.startsWith(QLatin1Char('/'))) {
        return QString();
    }

    QString prefix = baseUrl;
    while (prefix.endsWith(QLatin1Char('/'))) {
        prefix.chop(1);
    }
    if (prefix.isEmpty()) {
        return QString();
    }

    // 逐段编码：类别名直接进文件名，中文标签合法，裸拼对端拿到的是非法 URL
    const QStringList segments = rel.split(QLatin1Char('/'));
    QStringList encoded;
    encoded.reserve(segments.size());
    for (const QString &segment : segments) {
        encoded.append(QString::fromUtf8(QUrl::toPercentEncoding(segment)));
    }
    return prefix + QLatin1Char('/') + encoded.join(QLatin1Char('/'));
}

/// 把媒体预告摊成 JSON 子对象；未知字段（<0）整个省略而不是写 0，
/// 免得对端把"还不知道"读成"零字节文件"。
/// name/mime 为空同样省略：url 模式下片段的真实文件名与容器在报警那一刻都不知道
/// （submitAlarmClip 用它自己的时间戳拼 stem，猜一个只会在对端那里变成脏数据），
/// 这一刻能说的只有"media_id 已登记、available_in_ms 之后会有"。
static QJsonObject mediaRefToJson(const MediaRef &ref, const char *transfer)
{
    QJsonObject obj;
    obj[QStringLiteral("media_id")] = ref.mediaId;
    if (!ref.name.isEmpty()) {
        obj[QStringLiteral("name")] = ref.name;
    }
    if (!ref.mime.isEmpty()) {
        obj[QStringLiteral("mime")] = ref.mime;
    }
    obj[QStringLiteral("transfer")] = QString::fromLatin1(transfer);
    if (!ref.url.isEmpty()) {
        obj[QStringLiteral("url")] = ref.url;
    }
    obj[QStringLiteral("planned")] = ref.planned;
    if (ref.expectedBytes >= 0) {
        obj[QStringLiteral("expected_bytes")] = static_cast<double>(ref.expectedBytes);
    }
    if (ref.availableInMs >= 0) {
        obj[QStringLiteral("available_in_ms")] = ref.availableInMs;
    }
    return obj;
}

QJsonObject buildAlarmInfoMessage(const AlarmInfoInput &alarm,
                                  const QStringList &siblingAlarmIds,
                                  const MediaRef *snapshot,
                                  const MediaRef *clip,
                                  const QString &messageId,
                                  const QString &ts,
                                  const char *transfer)
{
    QJsonObject data;
    data[QStringLiteral("alarm_id")] = alarm.alarmId;
    QJsonArray ids;
    for (const QString &id : siblingAlarmIds) {
        ids.append(id);
    }
    data[QStringLiteral("alarm_ids")] = ids;
    data[QStringLiteral("alarm_type")] = alarm.alarmType;
    data[QStringLiteral("alarm_level")] = alarm.alarmLevel;
    data[QStringLiteral("alarm_time")] = alarm.alarmTime;
    // 内部/DB 是 0-based，UI 与文件名都是 channel+1：上线统一 1-based
    data[QStringLiteral("channel")] = alarm.channel + 1;
    data[QStringLiteral("class_id")] = alarm.classId;
    data[QStringLiteral("class_name")] = alarm.className;
    // float→double 直接升位会变成 0.8300000429153442 这种，取三位小数足够表达阈值
    data[QStringLiteral("confidence")] =
        qRound(static_cast<double>(alarm.confidence) * 1000.0) / 1000.0;
    data[QStringLiteral("is_fence_alarm")] = alarm.isFenceAlarm;
    data[QStringLiteral("status")] = alarm.status;
    if (snapshot) {
        data[QStringLiteral("snapshot")] = mediaRefToJson(*snapshot, transfer);
    }
    if (clip) {
        data[QStringLiteral("clip")] = mediaRefToJson(*clip, transfer);
    }

    QJsonObject envelope;
    envelope[QStringLiteral("type")] = QString::fromLatin1(kTypeAlarmInfo);
    envelope[QStringLiteral("message_id")] = messageId;
    envelope[QStringLiteral("ts")] = ts;
    envelope[QStringLiteral("data")] = data;
    return envelope;
}

QJsonObject buildMediaBeginMessage(const MediaJob &job,
                                   const ChunkPlan &plan,
                                   const QString &sha256Hex,
                                   const QString &messageId,
                                   const QString &ts)
{
    QJsonObject data;
    data[QStringLiteral("media_id")] = job.mediaId;
    data[QStringLiteral("alarm_id")] = job.alarmId;
    data[QStringLiteral("kind")] = job.kind;
    data[QStringLiteral("name")] = job.name;
    data[QStringLiteral("mime")] = job.mime;
    data[QStringLiteral("total_bytes")] = static_cast<double>(plan.totalBytes);
    data[QStringLiteral("chunk_size")] = plan.chunkSize;
    data[QStringLiteral("total_chunks")] = plan.totalChunks;
    data[QStringLiteral("sha256")] = sha256Hex;

    QJsonObject envelope;
    envelope[QStringLiteral("type")] = QString::fromLatin1(kTypeMediaBegin);
    envelope[QStringLiteral("message_id")] = messageId;
    envelope[QStringLiteral("ts")] = ts;
    envelope[QStringLiteral("data")] = data;
    return envelope;
}

QJsonObject buildMediaBeginUrlMessage(const MediaJob &job,
                                      const QString &url,
                                      qint64 totalBytes,
                                      const QString &sha256Hex,
                                      const QString &messageId,
                                      const QString &ts)
{
    QJsonObject data;
    data[QStringLiteral("media_id")] = job.mediaId;
    data[QStringLiteral("alarm_id")] = job.alarmId;
    data[QStringLiteral("kind")] = job.kind;
    data[QStringLiteral("name")] = job.name;      // 落盘后的真实名，覆盖 alarm_info 里的猜测
    data[QStringLiteral("mime")] = job.mime;      // 同上：url 模式下片段一定是 .mp4，按后缀算出来的
    data[QStringLiteral("transfer")] = QString::fromLatin1(kTransferHttp);
    data[QStringLiteral("url")] = url;
    data[QStringLiteral("total_bytes")] = static_cast<double>(totalBytes);
    if (!sha256Hex.isEmpty()) {
        data[QStringLiteral("sha256")] = sha256Hex;   // 对端下载完能自校验；算不出来就整个省略
    }

    QJsonObject envelope;
    envelope[QStringLiteral("type")] = QString::fromLatin1(kTypeMediaBegin);
    envelope[QStringLiteral("message_id")] = messageId;
    envelope[QStringLiteral("ts")] = ts;
    envelope[QStringLiteral("data")] = data;
    return envelope;
}

QJsonObject buildMediaFailedMessage(const QString &mediaId,
                                    const QString &alarmId,
                                    const QString &kind,
                                    const QString &reason,
                                    const QString &messageId,
                                    const QString &ts)
{
    QJsonObject data;
    data[QStringLiteral("media_id")] = mediaId;
    data[QStringLiteral("alarm_id")] = alarmId;
    data[QStringLiteral("kind")] = kind;
    data[QStringLiteral("reason")] = reason;      // 只写人机可读的原因，绝不含文件内容或字节

    QJsonObject envelope;
    envelope[QStringLiteral("type")] = QString::fromLatin1(kTypeMediaFailed);
    envelope[QStringLiteral("message_id")] = messageId;
    envelope[QStringLiteral("ts")] = ts;
    envelope[QStringLiteral("data")] = data;
    return envelope;
}

QJsonObject buildMediaEndMessage(const QString &mediaId,
                                 int seqsSent,
                                 int totalChunks,
                                 const QString &sha256Hex,
                                 const QString &messageId,
                                 const QString &ts)
{
    QJsonObject data;
    data[QStringLiteral("media_id")] = mediaId;
    data[QStringLiteral("seqs_sent")] = seqsSent;   // 实际发出的二进制帧数
    data[QStringLiteral("total_chunks")] = totalChunks;
    data[QStringLiteral("sha256")] = sha256Hex;

    QJsonObject envelope;
    envelope[QStringLiteral("type")] = QString::fromLatin1(kTypeMediaEnd);
    envelope[QStringLiteral("message_id")] = messageId;
    envelope[QStringLiteral("ts")] = ts;
    envelope[QStringLiteral("data")] = data;
    return envelope;
}

// ----------------------------------------------------------------------------
// AlarmUplinkEngine
// ----------------------------------------------------------------------------

void AlarmUplinkEngine::logMsg(const char *category, const QString &message) const
{
    if (logger_) {
        logger_(QLatin1String(category), message);
    }
}

AlarmUplinkEngine::MediaSlot AlarmUplinkEngine::touchSlot(const QString &kind,
                                                          const QString &batchKey,
                                                          const QString &alarmId,
                                                          bool *fresh)
{
    const QString key = kind + QLatin1Char('\x01') + batchKey;
    auto it = slots_.find(key);
    if (it == slots_.end()) {
        if (fresh) {
            *fresh = true;
        }
        MediaSlot slot;
        slot.mediaId = newMediaId(kind == QLatin1String(kKindSnapshot) ? "s-" : "c-");
        slot.alarmIds.append(alarmId);
        slots_.insert(key, slot);
        slotOrder_.push_back(key);
        // slot 键是每批报警的文件路径，基本不重复，所以必须自己封顶：只留最近
        // kMaxMediaSlots 个，淘汰最早登记的（那些批次的媒体早发完了）。
        // 代价：被淘汰后同一路径再进来会拿到新 media_id，对端按 sha256 去重。
        while (static_cast<int>(slotOrder_.size()) > kMaxMediaSlots) {
            slots_.remove(slotOrder_.front());
            slotOrder_.pop_front();
        }
        // 上面动过 hash（插入可能 rehash、淘汰会 remove），迭代器一律不再用，重新取值
        return slots_.value(key);
    }

    if (!it->alarmIds.contains(alarmId)) {
        it->alarmIds.append(alarmId);   // 就地改，不插入 → 不触发 rehash
    }
    return *it;
}

void AlarmUplinkEngine::trimQueueToFit()
{
    const int maxPending = qMax(1, settings_.maxPending);
    while (static_cast<int>(queue_.size()) >= maxPending) {
        // 与 SnapWriter(kMaxPending=4)/ClipWriter(kMaxPending=8) 同口径：丢最旧，不阻塞生产者
        const PendingItem dropped = queue_.front();
        queue_.pop_front();
        // 媒体项按 error 报：alarm_info 早已把那个 media_id 许给对端了，这里一丢，
        // 分块模式下对端永远等不到 begin、url 模式下对端会去取一个根本没人写出来的地址。
        // 丢一条 alarm_info 只是少一条报警，量级不同，别混在 warning 里刷
        const bool promisedMedia = dropped.kind == ItemKind::MediaChunks
                                   || dropped.kind == ItemKind::MediaUrl
                                   || dropped.kind == ItemKind::MediaFailed;
        logMsg(promisedMedia ? "error" : "warning",
               QStringLiteral("上行: 待发队列已满(%1)，丢弃最旧 %2=%3 alarm=%4 已发帧=%5%6")
                   .arg(maxPending)
                   .arg(dropped.media ? QStringLiteral("media") : QStringLiteral("info"))
                   .arg(dropped.itemId, dropped.alarmId)
                   .arg(dropped.cursor)
                   .arg(promisedMedia ? QStringLiteral("（该 media_id 已申报，对端会拿不到）")
                                      : QString()));
    }
}

void AlarmUplinkEngine::rememberClipPromise(const QString &alarmId, const QString &mediaId)
{
    if (!clipPromises_.contains(alarmId)) {
        clipPromises_.insert(alarmId, mediaId);
        promiseOrder_.push_back(alarmId);
        // 与 slot 表同一口径：只留最近的，被淘汰的那批如果对端后来真来了片段，
        // 就是"没承诺过"（宁可少发一段，也不发一个对端认不出的 media_id）
        while (static_cast<int>(promiseOrder_.size()) > kMaxClipPromises) {
            clipPromises_.remove(promiseOrder_.front());
            promiseOrder_.pop_front();
        }
        return;
    }
    clipPromises_.insert(alarmId, mediaId);   // 同 id 再来一次（slot 被淘汰后重建）就地覆盖
}

QString AlarmUplinkEngine::clipMediaIdFor(const QStringList &alarmIds) const
{
    for (const QString &id : alarmIds) {
        const QString mediaId = clipPromises_.value(id);
        if (!mediaId.isEmpty()) {
            return mediaId;   // 同批共享一个片段，取第一个认得的就够了
        }
    }
    return QString();
}

void AlarmUplinkEngine::forgetClipPromises(const QStringList &alarmIds)
{
    for (const QString &id : alarmIds) {
        clipPromises_.remove(id);
    }
    // 顺序表里也一起挤掉：它只用来决定"谁最老"，留着已清的 id 只是占位
    for (auto it = promiseOrder_.begin(); it != promiseOrder_.end();) {
        if (!clipPromises_.contains(*it)) {
            it = promiseOrder_.erase(it);
        } else {
            ++it;
        }
    }
}

SubmitResult AlarmUplinkEngine::submit(const AlarmInfoInput &alarm)
{
    SubmitResult result;
    if (alarm.alarmId.isEmpty()) {
        logMsg("warning",
               QStringLiteral("上行: 丢弃缺少 alarm_id 的报警 (type=%1)").arg(alarm.alarmType));
        return result;
    }

    // 批次键 = 共享的抓拍图路径。没图（截图关了或写盘失败）就只能一条一个批次，
    // 片段那一路在 videoClipAttached 里按 ids 再合回来
    const QString batchKey =
        alarm.imagePath.isEmpty() ? QStringLiteral("alarm:") + alarm.alarmId : alarm.imagePath;

    MediaRef snap;
    MediaRef clip;
    const MediaRef *snapPtr = nullptr;
    const MediaRef *clipPtr = nullptr;
    QStringList siblings;
    const char *transfer = settings_.mediaViaUrl ? kTransferHttp : kTransferChunks;
    // url 模式的地址前缀/根目录缺一个就什么都拼不出来 —— 此时两条媒体腿整个不申报，
    // alarm_info 只剩文本。判据只在这一处：AlarmUplink::attach() 负责把服务起没起、
    // 前缀填没填如实写进 Settings，不再自己关 sendSnapshot/sendClip（两处各关一次迟早对不上）
    const bool urlUsable = !settings_.mediaViaUrl
                           || (!settings_.mediaBaseUrl.isEmpty()
                               && !settings_.mediaRootDir.isEmpty());

    if (settings_.sendSnapshot && !alarm.imagePath.isEmpty() && urlUsable) {
        bool fresh = false;
        const MediaSlot slot = touchSlot(QLatin1String(kKindSnapshot), batchKey, alarm.alarmId, &fresh);
        snap.mediaId = slot.mediaId;
        snap.name = QFileInfo(alarm.imagePath).fileName();
        snap.mime = QString::fromLatin1(kMimeJpeg);
        snap.expectedBytes = kSnapshotExpectedBytes;
        // url 模式：路径此刻已经定死（SnapWriter 只是还没写完），所以地址当场给得出。
        // 地址算不出来（路径不在服务根目录下）就整段不申报——报一个没有 url 的 http 引用
        // 比不报更坏：对端会一直等一个永远不会来的 media_begin
        if (settings_.mediaViaUrl) {
            // ⚠ alarm.imagePath 是**随 cwd 的相对路径**（"data/alarms/日期/文件.jpg"），
            // 而 QDir(rootDir).relativeFilePath() 对相对入参是**按 rootDir 去解析**的，
            // 不绝对化就得到 "data/alarms/日期/…" 这种双重前缀，实测对端拿到的是 404。
            snap.url = mediaUrlFor(settings_.mediaBaseUrl, settings_.mediaRootDir,
                                   QFileInfo(alarm.imagePath).absoluteFilePath());
        }
        if (!settings_.mediaViaUrl || !snap.url.isEmpty()) {
            snapPtr = &snap;
            siblings = slot.alarmIds;
            result.snapshotMediaId = slot.mediaId;
            result.snapshotFirst = fresh;
        }
    }
    if (settings_.sendClip && urlUsable) {
        const MediaSlot slot = touchSlot(QLatin1String(kKindClip), batchKey, alarm.alarmId);
        clip.mediaId = slot.mediaId;
        // 片段路径要等 ClipWriter 到点 dump（报警后 2 秒+），这一刻只有 media_id，
        // 先按报警 id 记下来，videoClipAttached 回来时再认领
        rememberClipPromise(alarm.alarmId, slot.mediaId);
        result.clipMediaId = slot.mediaId;
        if (!settings_.mediaViaUrl) {
            // 分块帧模式：对端只靠这条报文认识这段媒体，先按命名规则猜一个名字与容器，
            // 真实值以 media_begin 为准（两处各取一次时间戳，毫秒可能差 1~2，不能当真值用）
            QString stem = QFileInfo(alarm.imagePath).completeBaseName();
            if (stem.isEmpty()) {
                stem = QStringLiteral("ch%1_clip").arg(alarm.channel + 1);
            }
            clip.name = stem;
            clip.mime = settings_.clipMime;
        }
        // url 模式下 name/mime 整个不给：片段文件名由 submitAlarmClip 用它自己的
        // 时间戳拼出来，报警侧拿不到，猜出来只会在对端变成脏数据；
        // 容器同理（编码器会从 hevc 退档到 h264），一切以 media_begin 为准
        clip.availableInMs = kClipAvailableInMs;
        clipPtr = &clip;
        if (siblings.isEmpty()) {
            siblings = slot.alarmIds;   // 截图那一路没启用时，合批关系由片段 slot 提供
        }
    }
    if (siblings.isEmpty()) {
        siblings.append(alarm.alarmId);
    }

    PendingItem item;
    item.kind = ItemKind::AlarmInfo;
    item.itemId = newMediaId("a-");
    item.alarmId = alarm.alarmId;
    const QJsonObject message = buildAlarmInfoMessage(
        alarm, siblings, snapPtr, clipPtr, item.itemId,
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), transfer);

    OutboundFrame frame;
    frame.text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    item.frames.append(frame);

    trimQueueToFit();
    result.messageId = item.itemId;
    queue_.push_back(std::move(item));
    return result;
}

QString AlarmUplinkEngine::submitMedia(const MediaJob &job, const QByteArray &payload,
                                       const QString &sha256Hex)
{
    if (job.mediaId.isEmpty() || payload.isEmpty()) {
        logMsg("warning", QStringLiteral("上行: 媒体段无可发字节 media=%1 kind=%2 name=%3")
                              .arg(job.mediaId, job.kind, job.name));
        return QString();
    }

    const ChunkPlan plan = planChunks(payload.size(), settings_.chunkSize);
    PendingItem item;
    item.kind = ItemKind::MediaChunks;
    item.itemId = job.mediaId;
    item.alarmId = job.alarmId;
    item.media = true;
    item.totalChunks = plan.totalChunks;
    item.sha256 = sha256Hex;

    const QString beginId = newMediaId("m-");
    const QString beginTs = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    OutboundFrame begin;
    begin.text = QString::fromUtf8(
        QJsonDocument(buildMediaBeginMessage(job, plan, sha256Hex, beginId, beginTs))
            .toJson(QJsonDocument::Compact));
    item.frames.append(begin);

    // 切块在这里发生：mid() 是真实拷贝，峰值 = 交进来的整个文件 + 切好的块
    // （一段 ≤1.5MB），队列上限把总占用压住。二进制一律 QByteArray——过一道
    // QString::fromUtf8 就会被非法 UTF-8 的替换字符吃掉，字节不再是原样
    for (int i = 0; i < plan.totalChunks; ++i) {
        OutboundFrame chunk;
        chunk.binary = true;
        chunk.bytes = payload.mid(static_cast<qint64>(i) * plan.chunkSize,
                                  i == plan.totalChunks - 1 ? plan.lastChunkBytes : plan.chunkSize);
        item.frames.append(chunk);
    }
    // media_end 不预渲染：末帧的 seqs_sent 要取实际发出去的二进制帧数；
    // 半截就被淘汰的段压根不发 end，对端按 sha256 和自己的超时判

    trimQueueToFit();
    queue_.push_back(std::move(item));
    logMsg("info", QStringLiteral("上行: 媒体入队 media=%1 kind=%2 字节=%3 块=%4 块大小=%5")
                       .arg(job.mediaId, job.kind)
                       .arg(payload.size())
                       .arg(plan.totalChunks)
                       .arg(plan.chunkSize));
    return job.mediaId;
}

QString AlarmUplinkEngine::submitMediaUrl(const MediaJob &job, const QString &url,
                                          qint64 sizeBytes, const QString &sha256Hex)
{
    if (job.mediaId.isEmpty()) {
        return QString();   // 连 media_id 都没有，说明调用方那条承诺根本没登记，没什么可撤的
    }
    if (url.isEmpty() || sizeBytes <= 0) {
        logMsg("warning", QStringLiteral("上行: 媒体无可发地址 media=%1 kind=%2 name=%3 字节=%4")
                              .arg(job.mediaId, job.kind, job.name)
                              .arg(sizeBytes));
        return QString();
    }

    // media=false 是关键：url 模式没有二进制帧要收尾。标成 true 会被 pumpOut() 的
    // "游标越界就补一条 media_end"造出 seqs_sent=0 的垃圾帧，还会让
    // restartInFlightMedia() 每次重连把同一段重复申报一遍
    PendingItem item;
    item.kind = ItemKind::MediaUrl;
    item.itemId = job.mediaId;
    item.alarmId = job.alarmId;
    item.media = false;

    OutboundFrame frame;
    frame.text = QString::fromUtf8(QJsonDocument(buildMediaBeginUrlMessage(
        job, url, sizeBytes, sha256Hex, newMediaId("m-"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)))
                                       .toJson(QJsonDocument::Compact));
    item.frames.append(frame);

    trimQueueToFit();
    queue_.push_back(std::move(item));
    logMsg("info", QStringLiteral("上行: 媒体地址入队 media=%1 kind=%2 字节=%3 名=%4")
                       .arg(job.mediaId, job.kind)
                       .arg(sizeBytes)
                       .arg(job.name));   // 只打名字与字节数，绝不打内容
    return job.mediaId;
}

void AlarmUplinkEngine::submitMediaFailed(const QString &mediaId, const QString &alarmId,
                                          const QString &kind, const QString &reason)
{
    if (mediaId.isEmpty()) {
        return;   // 没承诺过就不必撤
    }

    PendingItem item;
    item.kind = ItemKind::MediaFailed;
    item.itemId = mediaId;
    item.alarmId = alarmId;
    item.media = false;

    OutboundFrame frame;
    frame.text = QString::fromUtf8(QJsonDocument(buildMediaFailedMessage(
        mediaId, alarmId, kind, reason, newMediaId("m-"),
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)))
                                       .toJson(QJsonDocument::Compact));
    item.frames.append(frame);

    trimQueueToFit();
    queue_.push_back(std::move(item));
    logMsg("warning", QStringLiteral("上行: 媒体承诺落空 media=%1 kind=%2 原因=%3")
                          .arg(mediaId, kind, reason));
}

int AlarmUplinkEngine::pumpOut()
{
    if (!transport_ || queue_.empty() || !transport_->transportConnected()) {
        return 0;   // 断线：整条队列原地保留，重连后由调用方再泵一次
    }

    int completed = 0;
    while (!queue_.empty()) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        PendingItem &head = queue_.front();
        if (head.awaitingAck) {
            if (now < head.ackDeadlineMs) {
                break;   // 确认还没来，也不许后面的插队（顺序是协议的一部分）
            }
            if (!retryFront(QStringLiteral("等确认超时"))) {
                continue;   // 已判失败出队，接着看新的队首
            }
            break;           // 已置退避时刻
        }
        if (head.retryAfterMs > now) {
            break;           // 退避中，等 nextWaitMs() 指的那个点
        }
        if (!transport_->readyForMore()) {
            break;   // 写缓冲已到水位，等下一次定时器，绝不一次灌完
        }

        // 媒体项的帧序 = [begin, chunk × N]，游标越过末尾就是该发 end 的时刻
        const bool endFrame = head.media && head.cursor >= head.frames.size();
        OutboundFrame frame;
        if (endFrame) {
            const QJsonObject message = buildMediaEndMessage(
                head.itemId, head.binarySent, head.totalChunks, head.sha256, newMediaId("m-"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            frame.text = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
        } else {
            frame = head.frames.at(head.cursor);
        }

        const qint64 written = frame.binary ? transport_->sendBinary(frame.bytes)
                                            : transport_->sendText(frame.text);
        if (written <= 0) {
            if (!head.failLogged) {
                // 只报第一次：20ms 一次的重试把同一个失败刷成刷屏日志没意义
                head.failLogged = true;
                logMsg("warning", QStringLiteral("上行: 发送失败 %1=%2 游标=%3")
                                      .arg(head.media ? QStringLiteral("media") : QStringLiteral("info"))
                                      .arg(head.itemId).arg(head.cursor));
            }
            break;   // 保持顺序：这一帧没出去，后面的也不能抢先
        }

        if (frame.binary) {
            ++head.binarySent;
        }
        ++head.cursor;

        const bool itemDone = endFrame || (!head.media && head.cursor >= head.frames.size());
        if (itemDone) {
            if (settings_.expectAck) {
                // 帧都出去了不等于对端收全了：留在队首等 uplink_ack，
                // 到点没确认就按 retry 走一遍（这条链路默认关着，见 Settings::expectAck）
                head.awaitingAck = true;
                head.ackDeadlineMs = QDateTime::currentMSecsSinceEpoch()
                                   + qMax(1, settings_.ackTimeoutMs);
                logMsg("info", QStringLiteral("上行: %1 已发完，等确认 %2 帧=%3")
                                   .arg(head.media ? QStringLiteral("媒体段") : QStringLiteral("alarm_info"))
                                   .arg(head.itemId).arg(head.cursor));
                break;
            }
            logMsg("info", QStringLiteral("上行: %1 已发完 %2 帧=%3")
                               .arg(head.media ? QStringLiteral("媒体段") : QStringLiteral("alarm_info"))
                               .arg(head.itemId).arg(head.cursor));
            queue_.pop_front();   // pop 之后 head 失效，本轮不再碰它
            ++completed;
        }
    }
    return completed;
}

bool AlarmUplinkEngine::retryFront(const QString &why)
{
    if (queue_.empty()) {
        return false;
    }
    PendingItem &head = queue_.front();
    ++head.attempts;
    head.awaitingAck = false;
    if (head.attempts > qMax(1, settings_.maxAttempts)) {
        logMsg("error", QStringLiteral("上行: %1 判失败并丢弃（重发 %2 次仍 %3）")
                            .arg(head.itemId).arg(head.attempts - 1).arg(why));
        queue_.pop_front();
        return false;
    }
    // 整段重发：媒体项回到第一块，信息项回到第 0 帧
    head.cursor = 0;
    head.binarySent = 0;
    head.failLogged = false;
    head.retryAfterMs = QDateTime::currentMSecsSinceEpoch()
                        + backoffMs(head.attempts - 1, kRetryBaseMs);
    logMsg("warning", QStringLiteral("上行: %1 第 %2 次重发（%3），退避 %4ms")
                          .arg(head.itemId).arg(head.attempts).arg(why)
                          .arg(backoffMs(head.attempts - 1, kRetryBaseMs)));
    return true;
}

void AlarmUplinkEngine::handleAck(const QString &messageId, const QString &mediaId,
                                  const QString &phase, const QString &status,
                                  const QString &error)
{
    if (queue_.empty()) {
        return;   // 迟到的确认：那一项早就超时重发或判失败丢了，没东西可认领
    }
    PendingItem &head = queue_.front();
    if (!head.awaitingAck) {
        return;   // 没在等确认（开关关着，或队首还没发完）
    }
    const QString claimed = phase == QLatin1String("media") ? mediaId : messageId;
    if (claimed != head.itemId) {
        logMsg("warning", QStringLiteral("上行: 确认对不上队首 声称=%1 队首=%2")
                              .arg(claimed, head.itemId));
        return;
    }

    if (status == QLatin1String("ok")) {
        head.awaitingAck = false;
        queue_.pop_front();
        logMsg("info", QStringLiteral("上行: %1 已确认").arg(head.itemId));
        return;
    }
    if (status == QLatin1String("reject")) {
        // 对端明确说"这个我不要"：重发只会再来一次 reject，直接丢
        logMsg("warning", QStringLiteral("上行: %1 被拒收（%2），丢弃")
                              .arg(head.itemId, error.isEmpty() ? QStringLiteral("无原因") : error));
        queue_.pop_front();
        return;
    }
    if (status == QLatin1String("retry")) {
        retryFront(error.isEmpty() ? QStringLiteral("对端要求重发") : error);
    }
}

int AlarmUplinkEngine::nextWaitMs() const
{
    if (queue_.empty()) {
        return 0;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const PendingItem &head = queue_.front();
    qint64 until = -1;
    if (head.awaitingAck) {
        until = head.ackDeadlineMs - now;
    } else if (head.retryAfterMs > now) {
        until = head.retryAfterMs - now;
    }
    if (until < 0) {
        return kFlushWaitMs;   // 只是水位没退下来，20ms 后再试
    }
    return static_cast<int>(qBound<qint64>(kFlushWaitMs, until, kBackoffCapMs));
}

void AlarmUplinkEngine::restartInFlightMedia()
{
    if (queue_.empty()) {
        return;
    }
    PendingItem &head = queue_.front();
    if (!head.media || head.cursor == 0) {
        return;
    }
    // 不做断点续传：半截流对端没法用。整段从第一块重发，靠 media_begin 的 sha256 去重
    logMsg("info", QStringLiteral("上行: 重连后整段重发 media=%1（此前已发 %2 帧）")
                       .arg(head.itemId).arg(head.cursor));
    head.cursor = 0;
    head.binarySent = 0;
    head.failLogged = false;
}

void AlarmUplinkEngine::clear()
{
    queue_.clear();
    // slot 表与片段承诺一起清：留着的话，重新接上链路时会给对端一个它没见过的新报警
    // 复用旧 media_id（旧那批的字节早就发完了），比重新生成更难解释
    slots_.clear();
    slotOrder_.clear();
    clipPromises_.clear();
    promiseOrder_.clear();
}

}  // namespace uplink
