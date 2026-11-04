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

/**
 * @brief 生成本引擎内部的各类 id（对端只当唯一键用，不解析内容）
 * @param prefix "a-"=alarm_info 的 message_id、"s-"=抓拍 media_id、"c-"=片段 media_id、
 *               "m-"=媒体相关报文（begin/end/failed）自己的 message_id。前缀由调用点决定，
 *               本函数不校验——写错只会让日志里的种类看不出来，不影响协议
 * @return 前缀 + 16 位十六进制（约 1.8e19 分之一空间内的碰撞概率可忽略）
 *
 * 取 UUID 的**前 16 个十六进制位**而不是整串：整串 32 位会把每条日志行撑宽一倍，
 * 而这些 id 的用途只是"对端去重 + 本机排障"，16 位在单机单进程的生成速率下够用。
 */
QString newMediaId(const char *prefix)
{
    // 36 位带连字符的 UUID → 去掉分隔符取前 16 位：够唯一，又不至于把日志行撑爆
    QString hex = QUuid::createUuid().toString(QUuid::WithoutBraces);
    hex.remove(QLatin1Char('-'));
    return QString::fromLatin1(prefix) + hex.left(16);
}

/**
 * @brief 把"文件多大"换算成"要发几帧"
 * @param totalBytes 真实字节数；<=0 返回全 0 计划（调用方据此当"没有媒体可发"）
 * @param chunkSize  单帧载荷，来自配置 alarm_uplink.chunk_size；这里只 qMax(1,·) 兜 0
 * @return 向上取整的块数，末块是余数，恒落在 [1, chunkSize]
 *
 * 这是全工程唯一的分块换算处（Qt 5.12 的 QWebSocket 不做出站分片，见 ChunkPlan 注释）。
 * 约束：totalChunks 是 int，(totalBytes + chunkSize - 1)/chunkSize 只在"MB 级文件 +
 * KB 级块"这个量级内安全——当前唯一调用方是 submitMedia，喂进来的是 MediaPump 读好的
 * 抓拍图(~365KB)与片段(≤1.5MB)。NOTE: 没有对 totalBytes 的上限检查，若将来用它切
 * 几十 MB 的文件要把块数改成 qint64 并重新核对对端上限。
 */
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
    // 施工信息走**嵌套子对象**，不把码里的键平铺进 data{}：
    // 平铺会和 alarm_id/channel/class_name 撞键，且码字段一改线上协议就跟着漂。
    // 空串（未登录）或不是对象（脏数据）时整个键省略，对端用"有没有 construction"
    // 直接判断这条报警有没有施工归属，比收到一个空对象更不容易搞错。
    if (!alarm.constructionJson.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(alarm.constructionJson.toUtf8());
        if (doc.isObject()) {
            data[QStringLiteral("construction")] = doc.object();
        }
    }
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
// 线程约定：整类**不加锁**。queue_/slots_/clipPromises_ 只允许在调用方所在的单一线程
// （产品里是 GUI 线程，测试里是主线程）上访问；submit/submitMedia*/pumpOut/handleAck
// 全部由 AlarmUplink 在 GUI 线程串起来调。跨线程的只有媒体字节本身——MediaPump 读好后
// 用 QMetaObject::invokeMethod 投回 GUI 线程才进这里（见 src/biz/ws/media_pump.cpp）。

void AlarmUplinkEngine::logMsg(const char *category, const QString &message) const
{
    if (logger_) {
        logger_(QLatin1String(category), message);
    }
}

/**
 * @brief 按 (kind, 批次键) 取媒体 slot：命中就复用并登记 alarmId，没命中就新建 media_id
 * @param kind        kKindSnapshot | kKindClip，决定 media_id 前缀 s-/c-
 * @param batchKey    同批报警共享的那份媒体的键（submit 里传的是抓拍图路径，没图时是 "alarm:<id>"）
 * @param fresh       可空出参：true = 本次新建（调用方据此决定"该去读盘了"，只读一次）
 * @return slot 的**值拷贝**，不是引用
 *
 * 键分隔符用 '\x01' 而不是 ':' 或 '/'：批次键本身是文件路径，路径里出现冒号（时间戳命名）
 * 很常见，用可见字符拼会撞键；'\x01' 不可能出现在 UTF-8 路径里。
 * 按值返回是必须的：下面插入/淘汰都会改动 QHash（rehash 让所有迭代器与引用失效），
 * 返回引用等于给调用方留一个悬垂口子。命中分支只做 alarmIds.append（不插入 → 不 rehash），
 * 所以那里敢用 it->。
 * 封顶淘汰的代价写在下面的行注释里：被淘汰的同一批次再来一次会拿到**新的** media_id，
 * 对端要靠 sha256 去重，这是明确接受的取舍（换取 slot 表不无界增长）。
 */
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

/**
 * @brief 入队前把队列压回 maxPending 以内（丢最旧）
 *
 * 必须在**每个** push 之前调用（submit / submitMedia / submitMediaUrl / submitMediaFailed
 * 四处都调），否则队列会在断线期间无界增长。qMax(1, settings_.maxPending) 让配置写成
 * 0 或负数时退化成"容量 1、每条挤掉上一条"，而不是死循环或整条链路停摆。
 * 副作用：被丢的媒体项**只记日志、不补发 media_failed**——对端手里确实会留一条
 * 永远兑现不了的 planned，这是"丢最旧"策略已知的后果（见下面 error 级日志的措辞），
 * 不是漏实现；真要闭环得把 dropped 的 media_id 再排一次队，而那又会先把队列填满。
 */
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

/**
 * @brief 按报警 id 反查它们在 alarm_info 里承诺过的片段 media_id
 * @return **第一个**非空命中；全空 = 这批报警没承诺过片段（调用方据此什么也不发）
 *
 * 同批多条共享同一个 media_id，所以取第一个认得的就够了——但如果调用方传进来的
 * ids 跨了两个批次（AlarmManager 的 ids 是按批给的，正常不会），这里只会认领前一批，
 * 后一批的承诺由 forgetClipPromises 清掉，不会再有 media_begin。
 * 表在引擎侧封顶淘汰（kMaxClipPromises），被淘汰的 id 走到这里就是"没承诺过"。
 */
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

// 必须在 clipMediaIdFor 之后调（先取 id 再清），反过来会先把承诺清掉、让调用方拿不到 id。
// 顺序表 promiseOrder_ 也在这里一起挤掉已清的 id：它只用来决定"谁最老"，
// 留着墓碑会让淘汰速度比实际承诺数快一倍以上。
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

/**
 * @brief 撤掉一段在 alarm_info 里承诺过的媒体，通知对端别再等
 * @param mediaId 空 = 这批根本没承诺过（上行接入前产生的报警、或 slot 已被淘汰），直接返回
 * @param reason  人机可读的中文原因，**绝不含文件内容或字节**
 *
 * 三个来源（都汇到 AlarmUplink 的对应槽里）：MediaPump 读盘失败/被挤掉、
 * ClipWriter 丢最旧、裸流或 .mp4 副本 dump 失败。
 * 为什么发报文放在引擎而不是调用方：两档 transport 都需要它——url 档更糟，
 * 对端会真的去取一个没人写出来的地址（404），只打本机日志等于把故障留给对端猜。
 * 队列仍可能把它挤掉（trimQueueToFit 里那条 error 日志），此时承诺就彻底没人撤，
 * 属已知残留：这条报文只有一帧、排在队尾，被挤掉的概率远低于媒体分块项。
 */
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

/**
 * @brief 唯一的发生动作：按发送水位把队首往外推
 * @return 本轮**完整发完并出队**的项数；半截的媒体项不计入（它的帧已部分发出，游标已推进）
 *
 * 四条不变量，改这段代码时必须一起保住：
 *   1. 严格顺序：只有队首会被发送，任何一帧失败（written<=0）就 break 保位，
 *      后面的项不许插队——顺序本身就是协议（begin → N 块 → end）。
 *   2. 水位闸门只在这里判 transport_->readyForMore()，调用方不需要再看写缓冲。
 *   3. 断线（transportConnected=false）直接返回 0，队列原地保留，重连后由
 *      AlarmUplink::onLinkConnected 再泵一次；本机不落库 outbox，进程重启即重新开始。
 *   4. media_end 不预渲染，只在这里游标越过 frames.size() 时现组（seqs_sent 要取
 *      **实际**发出的二进制帧数，与 total_chunks 不等就是对端该怀疑的信号）。
 *
 * expect_ack 打开时，项发完不出队而是转 awaitingAck 并设 deadline（默认关着：
 * 现在的 Pad 不回 uplink_ack，当成必经状态会把队列一直堵到超时）。
 *
 * 线程要求：只能在 GUI 线程调（与 socket 同线程），且**不可重入**——head 是对 deque
 * 首元素的引用，pop_front 之后立即失效，所以下面 pop 之后那几行绝不再碰 head。
 */
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

/**
 * @brief 队首走一次失败处理：还能重发就置退避时刻，超过 maxAttempts 就判失败出队
 * @param why 只用于日志（"等确认超时" / 对端给的 error），不参与判断
 * @return true = 已置退避（稍后整项重发）；false = 已丢弃出队，或队列本来就是空
 *
 * attempts 先自增再比较，所以实际发送次数是 maxAttempts+1（首发的第 0 次不计）。
 * 重发是**整项重头**（cursor/binarySent 归零），不做断点续传：半截流对端拼不出来。
 * 退避用 backoffMs(attempts-1, kRetryBaseMs) → 2/4/8/16/30s 封顶，
 * 这一段只在 expect_ack=true 时才走得到（发送失败本身不重试，靠下轮 pumpOut 保位）。
 */
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

/**
 * @brief 处理对端回来的 uplink_ack（只在 expect_ack=true 时有意义）
 * @param phase "media" 时用 mediaId 认领队首，其余一律用 messageId——**只与队首对账**，
 *              发送是严格顺序的，队首之外不可能有别的东西在等确认
 * @param status ok=出队；reject=出队并记 warning（重发只会再来一次 reject）；
 *               retry=走 retryFront；其他取值当作噪声忽略，不做默认重试
 *
 * 三种"对不上"的情况都是静默返回或一条 warning，不抛不断链：
 *   队列空 = 迟到的确认（那项早已超时重发或判失败丢弃）；
 *   队首没在 awaitingAck = 开关关着 / 队首还没发完；
 *   id 不等 = 对端实现写错，这条 warning 是唯一线索，所以不能省。
 * 副作用：认领成功后本函数会出队，调用方（AlarmUplink::onUplinkAcked）紧接着 pump 一次。
 */
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

/**
 * @brief 告诉调用方下一次 pumpOut 该等多久（定时器时长唯一的来源）
 * @return 0 = 队列空，调用方应停掉定时器；否则 [kFlushWaitMs, kBackoffCapMs] 之间的毫秒数
 *
 * 三种节奏的来源不同，别混成一个常数：等确认 → 剩下的 deadline；退避中 → 剩下的退避；
 * 都只是写缓冲水位没退下来 → kFlushWaitMs(20ms)。
 * qBound 的下限是刻意的：deadline 与 now 相等时算出 0，而 flushTimer_ 是单发定时器，
 * start(0) 会立刻回来再泵一次，等确认期间就变成事件循环里的空转。
 * 上限压到 kBackoffCapMs 只为了给出一个可预期的帽（ackTimeoutMs 被配成天文数字时，
 * 定时器不会挂到 int 溢出），实际节奏由队首决定。
 */
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

/**
 * @brief 重连后把"正在传"的那一段媒体退回第一块
 *
 * 只管队首一项：pumpOut 的严格顺序决定了**只可能有队首**处于半截发送状态，
 * 其余项的 cursor 都还是 0，不需要动。`head.cursor == 0` 因此既是"没发过"的判断，
 * 也顺带跳过了 alarm_info / media_failed 这些非媒体项。
 * 不做断点续传（半截流对端拼不出来），整段重发后靠 media_begin 里的 sha256 让对端去重；
 * url 模式的项 media=false，本来就不在这条路径上（地址重发一次没有副作用）。
 * 由 AlarmUplink::onLinkConnected 在每次 connected 信号里调，重复调用幂等。
 */
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

/**
 * @brief 清空全部状态：待发队列 + 媒体 slot 表 + 片段承诺表
 *
 * 三张表必须一起清（见函数体内注释），单独清队列会让下次 attach 复用旧 media_id。
 * 调用点是 AlarmUplink::detach()（退出流程）；运行期没有"只清队列"的入口。
 * 副作用：正在传的那一段被丢弃且不通知对端（detach 之后链路本来就要断了）。
 */
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
