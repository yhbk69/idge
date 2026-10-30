// ============================================================================
// test_alarm_uplink.cpp — 报警上行（alarm_info + 媒体分块 + 地址交付 + 读盘）的离线自检
// ----------------------------------------------------------------------------
// 只链 Qt5::Core + alarm_uplink_engine.cpp + media_payload.cpp：用 FakeTransport
// 顶掉 WsClient，因此不需要 WebSocket、不需要 GUI 线程、也不需要 AlarmManager。
// 覆盖十八条契约：
//   1) 报文形状：信封字段、channel 1-based、confidence 取三位、媒体子对象是"预告"
//   2) 合流：同一路径的三条报警共享同一 media_id，alarm_ids 随批增长
//   3) 队列上限：满了丢最旧，且丢的确实是最旧那几条
//   4) 未连接 / 超水位 / 写失败三种情况下队列原样保留，顺序不被插队
//   5) 开关：send_snapshot/send_clip 关掉时对应子对象整个缺席
//   6) slot 表封顶后同一路径会拿到新 media_id（文档里写明的代价，锁成行为）
//   7) 分块计划：826000 字节 @262144 = 4 块且末块是余数；chunkSize=0 不除零
//   8) 媒体段帧序 begin → N 块 → end，begin/end 的字节数、块数、seqs_sent、sha256 自洽
//   9) 半途而废后整段重发（不做断点续传），重发不产生半截文件
//  10) 读盘：sha256 与已知向量一致、空文件/缺失文件报错不重试、后缀换算 MIME
//  11) 片段承诺：media_id 只能按报警 id 找回，交出/淘汰后反查表要干净
//  12~17) url 那档（出厂默认）：alarm_info 里 transfer=http + 抓拍当场给地址、
//      片段只承诺不给地址；前缀为空则两条媒体腿整体不申报；media_begin(http) 没有
//      分块字段也**没有 media_end**；重连不重复申报（项必须标 media=false）；
//      挤掉已申报媒体要报 error；media_failed 形状；mediaUrlFor 的越界与百分号编码；
//      退避梯子 min(2^n×2s, 30s) 与 uplink_ack 的 ok/retry/reject/迟到四种对账结果
//  1~11 全建在 Settings 的默认值 mediaViaUrl=false（=ws_binary）之上，那一条链路是
//  "保留代码但默认关"的回归保护，改它们等于把旧链路失去验证。
//  18) 施工信息：作为 data["construction"] **嵌套子对象**原样上行，不平铺进 data{}
//      （平铺会和 alarm_id/channel/class_name 撞键，且码字段一改线上协议就跟着漂）；
//      未登录（空串）或脏数据（不是对象）时这个键**整个缺席**，不是给一个空对象。
// 仓库根运行：./build/build_rk3588_linux/test_alarm_uplink，rc 0=PASS 1=FAIL
// ============================================================================

#include "alarm_uplink_engine.h"
#include "media_payload.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPair>
#include <QVector>

#include <iostream>
#include <stdexcept>

using uplink::AlarmInfoInput;
using uplink::AlarmUplinkEngine;

static void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

/// 记录每一帧发出去的内容，返回值/水位/预算可注入以模拟失败、超水位、半途而废
class FakeTransport : public uplink::UplinkTransport
{
public:
    bool connected = true;
    bool ready = true;
    qint64 textResult = 1;   ///< <=0 模拟发送失败
    int frameBudget = -1;    ///< >=0 时：已发帧数到达该预算就不再接新帧（模拟写缓冲水位退不下去）

    struct Sent {
        QString text;
        QByteArray bytes;
    };
    QVector<Sent> sent;

    bool transportConnected() const override { return connected; }
    bool readyForMore() const override
    {
        if (frameBudget >= 0) {
            return static_cast<int>(sent.size()) < frameBudget;
        }
        return ready;
    }

    qint64 sendText(const QString &text) override
    {
        if (textResult <= 0) {
            return textResult;
        }
        sent.push_back({text, QByteArray()});
        return textResult;
    }
    qint64 sendBinary(const QByteArray &bytes) override
    {
        if (frameBudget >= 0 && static_cast<int>(sent.size()) >= frameBudget) {
            return -1;
        }
        sent.push_back({QString(), bytes});
        return bytes.size();
    }

    QJsonObject jsonAt(int index) const
    {
        return QJsonDocument::fromJson(sent.at(index).text.toUtf8()).object();
    }
    QJsonObject dataAt(int index) const { return jsonAt(index)["data"].toObject(); }
    int textCount() const
    {
        int n = 0;
        for (const Sent &s : sent) {
            if (!s.text.isEmpty()) ++n;
        }
        return n;
    }
    /// 第 n 个二进制帧（与 textCount 一样按各自种类计数）
    QByteArray binaryAt(int n) const
    {
        int seen = -1;
        for (const Sent &s : sent) {
            if (s.text.isEmpty() && ++seen == n) {
                return s.bytes;
            }
        }
        return QByteArray();
    }
    int binaryCount() const
    {
        int n = 0;
        for (const Sent &s : sent) {
            if (s.text.isEmpty()) ++n;
        }
        return n;
    }
    /// 某个 type 出现了几次（重连整段重发后 media_begin 会有两条）
    int countType(const char *type) const
    {
        int n = 0;
        for (const Sent &s : sent) {
            if (s.text.isEmpty()) continue;
            if (QJsonDocument::fromJson(s.text.toUtf8()).object()["type"].toString()
                == QLatin1String(type)) {
                ++n;
            }
        }
        return n;
    }
};

static AlarmInfoInput makeAlarm(const QString &alarmId, const QString &imagePath,
                                int channel = 0, float confidence = 0.83f)
{
    AlarmInfoInput a;
    a.alarmId = alarmId;
    a.alarmType = QStringLiteral("un_helmet");
    a.alarmLevel = 2;
    a.alarmTime = QStringLiteral("2026-10-29T10:11:12");
    a.channel = channel;
    a.classId = 0;
    a.className = QStringLiteral("person");
    a.confidence = confidence;
    a.isFenceAlarm = false;
    a.imagePath = imagePath;
    return a;
}

int main() try
{
    FakeTransport transport;
    AlarmUplinkEngine engine;
    QVector<QPair<QString, QString>> logs;
    engine.setTransport(&transport);
    engine.setLogger([&logs](const QString &cat, const QString &msg) {
        logs.append(qMakePair(cat, msg));
    });

    // ---- 1) 报文形状 ------------------------------------------------------
    const uplink::SubmitResult first = engine.submit(
        makeAlarm("A1", "/data/alarms/20261029/ch1_un_helmet_101112_345.jpg"));
    const QString mid = first.messageId;
    check(!mid.isEmpty(), "submit 应返回 message_id");
    check(mid.startsWith(QStringLiteral("a-")), "message_id 前缀应为 a-");
    check(first.snapshotFirst && !first.snapshotMediaId.isEmpty(),
          "首次见到的抓拍批次要报 first，调用方才知道该读盘");
    check(engine.pendingCount() == 1, "submit 只入队不发送");
    check(transport.textCount() == 0, "submit 不该直接发");

    engine.pumpOut();
    check(transport.textCount() == 1, "pumpOut 应发出 1 条");
    check(engine.pendingCount() == 0, "发送成功后应出队");

    const QJsonObject env = transport.jsonAt(0);
    check(env["type"].toString() == QLatin1String(uplink::kTypeAlarmInfo), "type 应为 alarm_info");
    check(env["message_id"].toString() == mid, "信封 message_id 应与返回值一致");
    const QString ts = env["ts"].toString();
    check(ts.contains(QLatin1Char('T')) && ts.endsWith(QLatin1Char('Z')), "ts 应是 UTC ISO8601");
    check(env.contains("data") && env["data"].isObject(), "负载必须嵌在 data 里");

    const QJsonObject data = env["data"].toObject();
    check(data["alarm_id"].toString() == QStringLiteral("A1"), "alarm_id 丢失");
    check(data["channel"].toInt() == 1, "channel 上线必须 1-based（内部 0）");
    check(data["alarm_level"].toInt() == 2, "alarm_level 丢失");
    check(data["is_fence_alarm"].toBool() == false, "is_fence_alarm 应为 false");
    check(data["status"].toString() == QStringLiteral("pending"), "status 默认 pending");
    check(qAbs(data["confidence"].toDouble() - 0.83) < 1e-9, "confidence 应取三位小数而非 float 升位");
    check(data["alarm_ids"].toArray().size() == 1, "首条报警的 alarm_ids 只有它自己");

    const QJsonObject snap = data["snapshot"].toObject();
    check(snap["media_id"].toString().startsWith(QStringLiteral("s-")), "抓拍 media_id 前缀应为 s-");
    check(snap["name"].toString() == QStringLiteral("ch1_un_helmet_101112_345.jpg"), "抓拍 name 取文件名");
    check(snap["mime"].toString() == QLatin1String(uplink::kMimeJpeg), "抓拍 mime 应为 image/jpeg");
    check(snap["transfer"].toString() == QLatin1String(uplink::kTransferChunks), "transfer 应为 binary_chunks");
    check(snap["planned"].toBool(), "报警时刻文件还没落盘，必须标 planned");
    check(snap["expected_bytes"].toDouble() > 0, "抓拍应带预告字节数");
    check(!snap.contains("available_in_ms"), "未知的预告字段应整个省略而不是写 0");

    const QJsonObject clip = data["clip"].toObject();
    check(clip["media_id"].toString().startsWith(QStringLiteral("c-")), "片段 media_id 前缀应为 c-");
    check(clip["name"].toString() == QStringLiteral("ch1_un_helmet_101112_345"), "片段 name 用同前缀无扩展名");
    check(clip["available_in_ms"].toInt() == uplink::kClipAvailableInMs, "片段应带就绪时间预告");
    check(!clip.contains("expected_bytes"), "片段字节数此刻无从得知");
    // 未登录（或改动前的历史报警回填出来的行）没有施工信息，键必须整个缺席：
    // 给一个空对象会让对端把"没有归属"和"归属是空"当成同一件事
    check(!data.contains("construction"), "未带施工信息时 construction 键必须整个缺席");

    // ---- 1b) 施工信息：嵌套子对象，不平铺 ----------------------------------
    {
        FakeTransport peer;
        AlarmUplinkEngine engineC;
        engineC.setTransport(&peer);
        AlarmInfoInput c = makeAlarm("AC", "/data/alarms/20261029/ch1_un_helmet_101112_345.jpg");
        c.constructionJson = QStringLiteral(
            "{\"construction_id\":\"SG-2026-11\",\"worker_name\":\"张三\",\"team\":\"甲班\"}");
        engineC.submit(c);
        engineC.pumpOut();
        check(peer.textCount() == 1, "带施工信息的报警照常只发一条");
        const QJsonObject d = peer.jsonAt(0)["data"].toObject();
        check(d.contains("construction") && d["construction"].isObject(),
              "施工信息必须是 data 里的嵌套对象");
        const QJsonObject cobj = d["construction"].toObject();
        check(cobj["construction_id"].toString() == QStringLiteral("SG-2026-11"), "施工id 丢失");
        check(cobj["worker_name"].toString() == QStringLiteral("张三"), "负责人姓名丢失");
        check(cobj["team"].toString() == QStringLiteral("甲班"),
              "码里的未知键要原样透传（改字段时不动上行代码就是靠这条）");
        check(!d.contains("construction_id") && !d.contains("worker_name"),
              "不许把码里的键平铺进 data —— 会和 alarm_id/channel/class_name 撞键");

        // 脏数据（DB 里存着的旧值不是 JSON 对象）也不能把整条报警带崩
        FakeTransport dirtyPeer;
        AlarmUplinkEngine engineD;
        engineD.setTransport(&dirtyPeer);
        AlarmInfoInput dirty = makeAlarm("AD", "/data/alarms/20261029/ch1_un_helmet_101112_345.jpg");
        dirty.constructionJson = QStringLiteral("不是 JSON");
        engineD.submit(dirty);
        engineD.pumpOut();
        const QJsonObject dd = dirtyPeer.jsonAt(0)["data"].toObject();
        check(dd["alarm_id"].toString() == QStringLiteral("AD"), "脏施工信息不该影响报文其余部分");
        check(!dd.contains("construction"), "解不出对象时 construction 必须缺席");
    }

    // ---- 2) 按 imagePath 合流 --------------------------------------------
    AlarmUplinkEngine joined;
    FakeTransport joinedPeer;
    joined.setTransport(&joinedPeer);
    const QString shared = QStringLiteral("/data/alarms/20261029/ch1_un_helmet_101112_345.jpg");
    const uplink::SubmitResult r1 = joined.submit(makeAlarm("B1", shared));
    const uplink::SubmitResult r2 = joined.submit(makeAlarm("B2", shared));
    const uplink::SubmitResult r3 = joined.submit(makeAlarm("B3", shared));
    const uplink::SubmitResult r4 =
        joined.submit(makeAlarm("B4", QStringLiteral("/data/alarms/20261029/ch2_fire_101113_001.jpg")));
    // 同一批次只有第一条报 first：这就是"一张图只读一次、只上传一次"的判据
    check(r1.snapshotFirst && !r2.snapshotFirst && !r3.snapshotFirst && r4.snapshotFirst,
          "抓拍 first 只在批次首次出现时为真");
    check(r1.snapshotMediaId == r2.snapshotMediaId && r2.snapshotMediaId == r3.snapshotMediaId,
          "同批三行共享同一个抓拍 media_id");
    check(r4.snapshotMediaId != r1.snapshotMediaId, "不同批次必须是不同 media_id");
    check(joined.pendingCount() == 4, "四条报警各发一条 alarm_info");
    joined.pumpOut();
    check(joinedPeer.textCount() == 4, "应发出四条");

    const QJsonObject d1 = joinedPeer.dataAt(0);
    const QJsonObject d3 = joinedPeer.dataAt(2);
    check(d1["snapshot"].toObject()["media_id"] == d3["snapshot"].toObject()["media_id"],
          "同一路径必须复用同一抓拍 media_id");
    check(d1["clip"].toObject()["media_id"] == d3["clip"].toObject()["media_id"],
          "同一路径必须复用同一片段 media_id");
    check(d1["alarm_ids"].toArray().size() == 1, "第一条只知道它自己");
    check(d3["alarm_ids"].toArray().size() == 3, "第三条应回指同批三条");
    const QJsonArray ids3 = d3["alarm_ids"].toArray();
    check(ids3.at(0).toString() == QStringLiteral("B1") && ids3.at(2).toString() == QStringLiteral("B3"),
          "alarm_ids 应按到达顺序");
    check(d3["alarm_id"].toString() == QStringLiteral("B3"), "alarm_id 仍是本条自己");
    const QJsonObject d4 = joinedPeer.dataAt(3);
    check(d4["snapshot"].toObject()["media_id"].toString()
              != d3["snapshot"].toObject()["media_id"].toString(),
          "不同路径必须是不同的 media_id");

    // ---- 3) 队列上限：满了丢最旧 ------------------------------------------
    AlarmUplinkEngine capped;
    FakeTransport cappedPeer;
    cappedPeer.connected = false;
    capped.setTransport(&cappedPeer);
    QVector<QPair<QString, QString>> capLogs;
    capped.setLogger([&capLogs](const QString &cat, const QString &msg) {
        capLogs.append(qMakePair(cat, msg));
    });
    AlarmUplinkEngine::Settings settings;
    settings.maxPending = 3;
    capped.setSettings(settings);
    for (int i = 1; i <= 5; ++i) {
        capped.submit(makeAlarm(QStringLiteral("C%1").arg(i),
                                QStringLiteral("/p/%1.jpg").arg(i)));
    }
    check(capped.pendingCount() == 3, "队列应被压在上限 3");
    int dropped = 0;
    for (const auto &entry : capLogs) {
        if (entry.first == QLatin1String("warning") && entry.second.contains(QStringLiteral("丢弃最旧"))) {
            ++dropped;
        }
    }
    check(dropped == 2, "应记录两次丢弃最旧");
    check(cappedPeer.textCount() == 0, "未连接时一条都不该发");
    check(capped.hasPending(), "未连接时队列必须保留");
    cappedPeer.connected = true;
    check(capped.pumpOut() == 3, "重连后补发全部积压");
    check(cappedPeer.dataAt(0)["alarm_id"].toString() == QStringLiteral("C3"),
          "留下的应是最晚那三条里的第一条");
    check(cappedPeer.dataAt(2)["alarm_id"].toString() == QStringLiteral("C5"), "顺序按到达");

    // ---- 4) 水位闸门与写失败：不插队 --------------------------------------
    AlarmUplinkEngine gated;
    FakeTransport gatedPeer;
    gatedPeer.ready = false;
    gated.setTransport(&gatedPeer);
    gated.submit(makeAlarm("D1", "/p/d1.jpg"));
    gated.submit(makeAlarm("D2", "/p/d2.jpg"));
    check(gated.pumpOut() == 0, "超水位时不发");
    check(gated.pendingCount() == 2, "超水位时队列不动");
    gatedPeer.ready = true;
    check(gated.pumpOut() == 2, "水位退下后全部发出");
    check(gatedPeer.dataAt(0)["alarm_id"].toString() == QStringLiteral("D1"), "水位恢复后仍按原顺序");

    AlarmUplinkEngine broken;
    FakeTransport brokenPeer;
    brokenPeer.textResult = -1;
    broken.setTransport(&brokenPeer);
    broken.submit(makeAlarm("E1", "/p/e1.jpg"));
    broken.submit(makeAlarm("E2", "/p/e2.jpg"));
    check(broken.pumpOut() == 0, "写失败不算发出");
    check(broken.pendingCount() == 2, "写失败后原样排队");
    brokenPeer.textResult = 7;   // 正数即视为已交付（Qt 的 sendTextMessage 没有半帧）
    check(broken.pumpOut() == 2, "恢复后应把两条都发出去");
    check(brokenPeer.dataAt(0)["alarm_id"].toString() == QStringLiteral("E1"), "失败的那条不能被插队");

    // ---- 5) 开关：子对象整个缺席 ------------------------------------------
    AlarmUplinkEngine minimal;
    FakeTransport minimalPeer;
    minimal.setTransport(&minimalPeer);
    AlarmUplinkEngine::Settings noMedia;
    noMedia.sendSnapshot = false;
    noMedia.sendClip = false;
    minimal.setSettings(noMedia);
    minimal.submit(makeAlarm("F1", "/p/f1.jpg"));
    minimal.pumpOut();
    const QJsonObject fData = minimalPeer.dataAt(0);
    check(!fData.contains("snapshot"), "send_snapshot=false 时不该申报抓拍");
    check(!fData.contains("clip"), "recording 关闭时上层会把 sendClip 折算成 false");
    check(fData["alarm_ids"].toArray().size() == 1, "没有媒体 slot 时 alarm_ids 仍含自身");

    // 只有片段一路（截图关了）：合批关系由片段 slot 提供
    AlarmUplinkEngine clipOnly;
    FakeTransport clipOnlyPeer;
    clipOnly.setTransport(&clipOnlyPeer);
    AlarmUplinkEngine::Settings clipSettings;
    clipSettings.sendSnapshot = false;
    clipOnly.setSettings(clipSettings);
    clipOnly.submit(makeAlarm("G1", "/p/g.jpg"));
    clipOnly.submit(makeAlarm("G2", "/p/g.jpg"));
    clipOnly.pumpOut();
    check(clipOnlyPeer.dataAt(1)["alarm_ids"].toArray().size() == 2,
          "只发片段时同批仍要合流");

    // ---- 6) 脏数据与 slot 封顶 -------------------------------------------
    AlarmUplinkEngine dirty;
    FakeTransport dirtyPeer;
    QVector<QPair<QString, QString>> dirtyLogs;
    dirty.setTransport(&dirtyPeer);
    dirty.setLogger([&dirtyLogs](const QString &cat, const QString &msg) {
        dirtyLogs.append(qMakePair(cat, msg));
    });
    const uplink::SubmitResult dirtyRes = dirty.submit(makeAlarm(QString(), "/p/x.jpg"));
    check(dirtyRes.messageId.isEmpty(), "没有 alarm_id 应直接丢弃");
    check(dirtyRes.snapshotMediaId.isEmpty() && !dirtyRes.snapshotFirst,
          "被丢弃的脏数据不该申报任何媒体（否则会白读一次盘）");
    check(dirty.pendingCount() == 0 && dirtyPeer.textCount() == 0, "脏数据既不入队也不发送");
    check(dirtyLogs.size() == 1 && dirtyLogs.at(0).first == QLatin1String("warning"), "丢弃要有一条 warning");

    AlarmUplinkEngine flooded;
    FakeTransport floodedPeer;
    flooded.setTransport(&floodedPeer);
    const QString evictedPath = QStringLiteral("/p/evict.jpg");
    flooded.submit(makeAlarm("H0", evictedPath));
    flooded.pumpOut();
    const QString firstId = floodedPeer.dataAt(0)["snapshot"].toObject()["media_id"].toString();
    for (int i = 1; i <= uplink::kMaxMediaSlots + 2; ++i) {
        flooded.submit(makeAlarm(QStringLiteral("H%1").arg(i),
                                 QStringLiteral("/p/%1.jpg").arg(i)));
    }
    flooded.clear();
    flooded.submit(makeAlarm("H9", evictedPath));
    flooded.pumpOut();
    const QString againId = floodedPeer.dataAt(floodedPeer.textCount() - 1)["snapshot"]
                                .toObject()["media_id"].toString();
    check(againId != firstId, "slot 表封顶后同路径会重新登记（文档写明的代价，此处锁成行为）");
    check(againId.startsWith(QStringLiteral("s-")), "淘汰后仍能生成合法 media_id");

    // ---- 7) 分块计划（一次 sendBinaryMessage = 一个不拆分的数据帧）---------
    {
        const uplink::ChunkPlan empty = uplink::planChunks(0, 262144);
        check(empty.totalChunks == 0 && empty.lastChunkBytes == 0, "空文件不该产生块");
        const uplink::ChunkPlan zero = uplink::planChunks(10, 0);
        check(zero.chunkSize == 1 && zero.totalChunks == 10, "chunkSize=0 必须被夹到 1 而不是除零");
        const uplink::ChunkPlan exact = uplink::planChunks(262144, 262144);
        check(exact.totalChunks == 1 && exact.lastChunkBytes == 262144, "整除时末块恰好一整块");
        const uplink::ChunkPlan frag = uplink::planChunks(262145, 262144);
        check(frag.totalChunks == 2 && frag.lastChunkBytes == 1, "多一个字节就多一块");
        const uplink::ChunkPlan clip = uplink::planChunks(826000, 262144);
        check(clip.totalChunks == 4, "826000 字节 @262144 应是 4 块");
        check(clip.lastChunkBytes == 826000 - 3 * 262144, "末块应是余数 39568");
        check(clip.chunkSize * (clip.totalChunks - 1) + clip.lastChunkBytes == 826000,
              "块大小 × 块数必须能还原总字节数");
    }

    // ---- 8) 媒体段：begin → N 块 → end -----------------------------------
    AlarmUplinkEngine media;
    FakeTransport mediaPeer;
    QVector<QPair<QString, QString>> mediaLogs;
    media.setTransport(&mediaPeer);
    media.setLogger([&mediaLogs](const QString &cat, const QString &msg) {
        mediaLogs.append(qMakePair(cat, msg));
    });
    AlarmUplinkEngine::Settings mediaSettings;
    mediaSettings.chunkSize = 262144;
    media.setSettings(mediaSettings);

    uplink::MediaJob job;
    job.mediaId = QStringLiteral("s-clip0001");
    job.alarmId = QStringLiteral("A1");
    job.kind = QString::fromLatin1(uplink::kKindSnapshot);
    job.name = QStringLiteral("ch1_un_helmet_101112_345.jpg");
    job.mime = QString::fromLatin1(uplink::kMimeJpeg);
    const QByteArray clipBytes(826000, '\x5a');
    const QString sha = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    check(media.submitMedia(job, QByteArray(), sha).isEmpty(), "空载荷不产生媒体项");
    check(media.pendingCount() == 0, "空载荷没进队列");
    check(!mediaLogs.isEmpty() && mediaLogs.last().first == QLatin1String("warning"),
          "空载荷要有一条 warning，否则现场看不出为什么没发");

    check(!media.submitMedia(job, clipBytes, sha).isEmpty(), "submitMedia 返回 media_id");
    check(media.pendingCount() == 1, "一段媒体 = 一个待发项");
    check(media.pumpOut() == 1, "整段一次泵完");
    check(mediaPeer.textCount() == 2 && mediaPeer.binaryCount() == 4, "帧序 = 2 文本 + 4 二进制");

    const QJsonObject beginEnv = mediaPeer.jsonAt(0);
    check(beginEnv["type"].toString() == QLatin1String(uplink::kTypeMediaBegin), "首帧是 media_begin");
    const QJsonObject begin = beginEnv["data"].toObject();
    check(begin["media_id"].toString() == job.mediaId, "media_begin 必须回指 alarm_info 里那个 id");
    check(begin["alarm_id"].toString() == job.alarmId, "media_begin 少了归属报警");
    check(begin["kind"].toString() == QLatin1String(uplink::kKindSnapshot), "kind 应为 snapshot");
    check(begin["name"].toString() == job.name, "name 用落盘后的真实文件名");
    check(begin["total_bytes"].toDouble() == 826000.0, "total_bytes 取实际读到的字节数");
    check(begin["chunk_size"].toInt() == 262144 && begin["total_chunks"].toInt() == 4,
          "chunk_size × total_chunks 与 total_bytes 必须自洽");
    check(begin["sha256"].toString() == sha, "sha256 原样带上，对端据此校验");

    check(mediaPeer.binaryAt(0).size() == 262144 && mediaPeer.binaryAt(1).size() == 262144
              && mediaPeer.binaryAt(2).size() == 262144 && mediaPeer.binaryAt(3).size() == 39568,
          "四个二进制帧的字节数应按计划切分");
    {
        int total = 0;
        for (int i = 0; i < 4; ++i) {
            total += mediaPeer.binaryAt(i).size();
        }
        check(total == 826000, "分块拼回去必须等于原文件长度，不多不少");
        check(mediaPeer.binaryAt(0).at(0) == '\x5a', "字节内容必须原样透传，不被编码改写");
    }

    const QJsonObject endEnv = mediaPeer.jsonAt(mediaPeer.sent.size() - 1);
    check(endEnv["type"].toString() == QLatin1String(uplink::kTypeMediaEnd), "末帧是 media_end");
    const QJsonObject end = endEnv["data"].toObject();
    check(end["media_id"].toString() == job.mediaId, "media_end 对不上 media_id");
    check(end["seqs_sent"].toInt() == 4 && end["total_chunks"].toInt() == 4,
          "seqs_sent 是实际发出的块数，与计划相等才算完整");
    check(end["sha256"].toString() == sha, "media_end 要再给一次哈希让对端自检");

    // ---- 9) 半途而废：重连后整段重发，不做断点续传 ------------------------
    AlarmUplinkEngine half;
    FakeTransport halfPeer;
    half.setTransport(&halfPeer);
    AlarmUplinkEngine::Settings halfSettings;
    halfSettings.chunkSize = 262144;
    half.setSettings(halfSettings);
    half.submitMedia(job, clipBytes, sha);
    halfPeer.frameBudget = 3;   // begin + 2 块之后水位就不允许再发
    check(half.pumpOut() == 0, "半截的媒体项不算发完");
    check(half.hasPending(), "没发完必须留在队头");
    check(halfPeer.sent.size() == 3, "水位闸门应按帧生效，一次灌完等于没有闸门");

    halfPeer.frameBudget = -1;
    half.restartInFlightMedia();
    check(half.pumpOut() == 1, "整段重发后才算发完");
    check(halfPeer.countType(uplink::kTypeMediaBegin) == 2, "重连整段重发：begin 会出现两次");
    check(halfPeer.binaryCount() == 6, "重发的是完整 4 块，不是续传剩下的 2 块");
    const QJsonObject halfEnd = halfPeer.jsonAt(halfPeer.sent.size() - 1)["data"].toObject();
    check(halfEnd["seqs_sent"].toInt() == 4, "media_end 的 seqs_sent 只数本轮实际发出的块");

    // ---- 10) 读盘：sha256 / 空文件 / 缺失文件 / MIME ----------------------
    {
        const QString dir = QDir::tempPath() + QStringLiteral("/idge_uplink_selftest");
        QDir().mkpath(dir);
        const QString known = dir + QStringLiteral("/known_vector.txt");
        {
            QFile f(known);
            check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "测试用临时文件写不进去");
            f.write("abc");
            f.close();
        }
        uplink::MediaPayload payload;
        QString error;
        // "abc" 的 sha256 是公开测试向量：哈希实现写错就会被这条抓住
        check(uplink::loadMediaPayload(QStringLiteral("s-k"), QStringLiteral("A1"),
                                      QLatin1String(uplink::kKindSnapshot), known,
                                      QString::fromLatin1(uplink::kMimeJpeg), 3000,
                                      &payload, &error),
              "已写稳的文件应能读出来");
        check(payload.sha256Hex
                  == QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
              "sha256 与已知向量不一致");
        check(payload.name == QStringLiteral("known_vector.txt"), "name 取真实文件名");
        check(payload.mime == QLatin1String(uplink::kMimeJpeg),
              "认不出的后缀（.txt）应落回兜底 MIME，而不是留空");
        check(payload.sha256Hex.size() == 64, "sha256 应是 64 位十六进制");
        check(error.isEmpty(), "成功路径不该带 error");

        const QString big = dir + QStringLiteral("/big.h265");
        {
            QFile f(big);
            check(f.open(QIODevice::WriteOnly | QIODevice::Truncate), "大文件写不进去");
            f.write(QByteArray(300000, '\x42'));
            f.close();
        }
        check(uplink::loadMediaPayload(QStringLiteral("c-big"), QStringLiteral("A2"),
                                      QLatin1String(uplink::kKindClip), big,
                                      QString::fromLatin1(uplink::kMimeHevc), 3000,
                                      &payload, &error),
              "300000 字节的裸流应读全");
        check(payload.data.size() == 300000, "读到的字节数必须等于文件长度（截断就在这儿露出来）");
        check(payload.mime == QLatin1String(uplink::kMimeHevc), "h265 后缀换算成 video/H265");

        const QString zeroLen = dir + QStringLiteral("/empty.jpg");
        {
            QFile f(zeroLen);
            f.open(QIODevice::WriteOnly | QIODevice::Truncate);
            f.close();
        }
        check(!uplink::loadMediaPayload(QStringLiteral("s-e"), QStringLiteral("A3"),
                                        QLatin1String(uplink::kKindSnapshot), zeroLen,
                                        QString(), 200, &payload, &error),
              "零字节文件按失败处理（等不到 size>0）");
        check(error.contains(QStringLiteral("超时")), "失败原因要写清是等不到文件");

        check(!uplink::loadMediaPayload(QStringLiteral("s-m"), QStringLiteral("A4"),
                                        QLatin1String(uplink::kKindSnapshot),
                                        dir + QStringLiteral("/never_appears.jpg"),
                                        QString(), 0, &payload, &error),
              "不存在的路径直接失败，不重试");
        check(uplink::mimeForMediaPath(QStringLiteral("/a/b.h264"), QString())
                  == QLatin1String(uplink::kMimeAvc),
              "退档写出的 h264 裸流要报对 MIME，不能只认配置");
        check(uplink::mimeForMediaPath(QStringLiteral("/a/b.mp4"), QString())
                  == QStringLiteral("video/mp4"), "mp4 副本走 video/mp4");
        check(uplink::mimeForMediaPath(QStringLiteral("/a/b.bin"),
                                       QString::fromLatin1(uplink::kMimeJpeg))
                  == QLatin1String(uplink::kMimeJpeg), "认不出的后缀落回兜底值");
        QFile::remove(known);
        QFile::remove(big);
        QFile::remove(zeroLen);
    }

    // ---- 11) 片段承诺：media_id 只能按报警 id 找回 ------------------------
    {
        AlarmUplinkEngine clip;
        FakeTransport clipPeer;
        clip.setTransport(&clipPeer);
        const uplink::SubmitResult r = clip.submit(makeAlarm("K1", "/p/k.jpg"));
        check(r.clipMediaId.startsWith(QStringLiteral("c-")), "submit 应带回承诺过的片段 media_id");
        check(clip.clipMediaIdFor(QStringList{QStringLiteral("K1")}) == r.clipMediaId,
              "片段落盘时只有报警 id，media_id 得能从这儿找回");
        check(clip.clipMediaIdFor(QStringList{QStringLiteral("NOPE"), QStringLiteral("K1")})
                  == r.clipMediaId,
              "同批多个 id，认得第一个就够（一个片段对一整批）");
        check(clip.clipMediaIdFor(QStringList{QStringLiteral("NOPE")}).isEmpty(),
              "没承诺过的报警返回空，不能瞎给一个 media_id");

        clip.forgetClipPromises(QStringList{QStringLiteral("K1")});
        check(clip.clipMediaIdFor(QStringList{QStringLiteral("K1")}).isEmpty(),
              "交出去之后要清掉，免得同一段被认领第二次");

        // 封顶淘汰：每条报警都进这张表，不封顶就是无界增长
        QVector<QString> ids;
        for (int i = 0; i <= uplink::kMaxClipPromises; ++i) {
            const QString id = QStringLiteral("Q%1").arg(i);
            ids.append(id);
            clip.submit(makeAlarm(id, QStringLiteral("/p/q%1.jpg").arg(i)));
        }
        check(clip.clipMediaIdFor(QStringList{ids.first()}).isEmpty(), "最早的承诺应被淘汰");
        check(!clip.clipMediaIdFor(QStringList{ids.last()}).isEmpty(), "最新承诺仍在");

        // 截图关、只发片段：承诺照样登记（否则这一路永远发不出字节）
        AlarmUplinkEngine snapOff;
        FakeTransport snapOffPeer;
        snapOff.setTransport(&snapOffPeer);
        AlarmUplinkEngine::Settings clipOnlySettings;
        clipOnlySettings.sendSnapshot = false;
        snapOff.setSettings(clipOnlySettings);
        const uplink::SubmitResult snapOffRes = snapOff.submit(makeAlarm("S1", "/p/s.jpg"));
        check(snapOffRes.snapshotMediaId.isEmpty() && !snapOffRes.clipMediaId.isEmpty(),
              "关截图时抓拍不该有 media_id，片段仍要承诺");

        // 片段那一路被上层折算成 false（recording.enabled=false）：不该留任何承诺
        AlarmUplinkEngine noClip;
        FakeTransport noClipPeer;
        noClip.setTransport(&noClipPeer);
        AlarmUplinkEngine::Settings noClipSettings;
        noClipSettings.sendClip = false;
        noClip.setSettings(noClipSettings);
        const uplink::SubmitResult noClipRes = noClip.submit(makeAlarm("N1", "/p/n.jpg"));
        check(noClipRes.clipMediaId.isEmpty(), "不录像是压根没有片段，别向对端承诺");
        check(noClip.clipMediaIdFor(QStringList{QStringLiteral("N1")}).isEmpty(),
              "没承诺就不该进反查表");
    }

    // ---- 12) url 模式（出厂默认那档）：alarm_info 里给的是地址 -------------
    {
        const QString root = QStringLiteral("/data/alarms");
        const QString base = QStringLiteral("http://10.0.0.5:8090");
        AlarmUplinkEngine::Settings urlSettings;
        urlSettings.mediaViaUrl = true;
        urlSettings.mediaBaseUrl = base;
        urlSettings.mediaRootDir = root;

        AlarmUplinkEngine urlMode;
        FakeTransport urlPeer;
        urlMode.setTransport(&urlPeer);
        urlMode.setSettings(urlSettings);

        const uplink::SubmitResult u = urlMode.submit(
            makeAlarm("U1", root + QStringLiteral("/20261029/ch1_un_helmet_101112_345.jpg")));
        check(u.snapshotFirst && !u.snapshotMediaId.isEmpty(),
              "url 模式照样要登记抓拍 slot，调用方才知道该去读一次盘");
        check(!u.clipMediaId.isEmpty(), "片段照样要承诺，只是地址晚给");
        urlMode.pumpOut();

        const QJsonObject uData = urlPeer.dataAt(0);
        const QJsonObject uSnap = uData["snapshot"].toObject();
        check(uSnap["transfer"].toString() == QLatin1String(uplink::kTransferHttp),
              "url 模式的 transfer 必须是 http，对端据此决定是下载还是等帧");
        check(uSnap["url"].toString()
                  == base + QStringLiteral("/20261029/ch1_un_helmet_101112_345.jpg"),
              "抓拍路径此刻已定死，地址当场就给得出");
        check(uSnap["planned"].toBool(), "SnapWriter 还没写完，planned 仍是 true");
        check(uSnap["expected_bytes"].toDouble() > 0, "抓拍仍带预告字节数");

        const QJsonObject uClip = uData["clip"].toObject();
        check(uClip["transfer"].toString() == QLatin1String(uplink::kTransferHttp),
              "片段 transfer 同为 http");
        check(!uClip.contains("url"),
              "片段此刻给不出地址：文件名由 submitAlarmClip 自己拼时间戳，猜出来是脏数据");
        check(!uClip.contains("name") && !uClip.contains("mime"),
              "url 模式不猜片段的名字与容器（编码器还会从 hevc 退档到 h264）");
        check(uClip["available_in_ms"].toInt() == uplink::kClipAvailableInMs,
              "片段仍带就绪时间预告，对端有个预期");

        // ---- 13) 地址前缀为空 = 两条媒体腿整体不申报 ------------------------
        AlarmUplinkEngine noBase;
        FakeTransport noBasePeer;
        noBase.setTransport(&noBasePeer);
        AlarmUplinkEngine::Settings noBaseSettings = urlSettings;
        noBaseSettings.mediaBaseUrl.clear();   // 服务没起来 / 没有可用 IPv4 且没配前缀
        noBase.setSettings(noBaseSettings);
        const uplink::SubmitResult v = noBase.submit(makeAlarm("V1", root + QStringLiteral("/v.jpg")));
        check(v.snapshotMediaId.isEmpty() && !v.snapshotFirst && v.clipMediaId.isEmpty(),
              "拼不出地址就整段不申报：白读一次盘 + 一条永远兑现不了的空头承诺");
        check(noBase.clipMediaIdFor(QStringList{QStringLiteral("V1")}).isEmpty(),
              "没申报的片段不该进承诺反查表");
        noBase.pumpOut();
        const QJsonObject vData = noBasePeer.dataAt(0);
        check(!vData.contains("snapshot") && !vData.contains("clip"),
              "服务不在就只发报警文本，不申报任何媒体");
        check(vData["alarm_id"].toString() == QStringLiteral("V1"),
              "媒体给不出不能把报警本体也改形状");

        // 抓拍路径不在服务根目录之下（例如截图挪到了别的盘）：宁可这段不报
        AlarmUplinkEngine outside;
        FakeTransport outsidePeer;
        outside.setTransport(&outsidePeer);
        outside.setSettings(urlSettings);
        const uplink::SubmitResult w = outside.submit(makeAlarm("W1", "/elsewhere/w.jpg"));
        check(w.snapshotMediaId.isEmpty(), "root 之外的路径不该被零鉴权服务暴露出去");
        outside.pumpOut();
        check(!outsidePeer.dataAt(0).contains("snapshot"), "算不出地址的抓拍整个子对象缺席");

        // 现场 AlarmRecord::imagePath 是**随 cwd 的相对路径**（"data/alarms/日期/…"），
        // 而 QDir(root).relativeFilePath(相对入参) 是按 root 而不是按 cwd 解析的：
        // 不绝对化就会拼出 `http://…/data/alarms/20261029/…jpg`，板端实测 404。
        const QString cwdRoot =
            QDir::current().absoluteFilePath(QStringLiteral("data/alarms"));
        AlarmUplinkEngine::Settings relSettings;
        relSettings.mediaViaUrl = true;
        relSettings.mediaBaseUrl = base;
        relSettings.mediaRootDir = cwdRoot;
        AlarmUplinkEngine relMode;
        FakeTransport relPeer;
        relMode.setTransport(&relPeer);
        relMode.setSettings(relSettings);
        relMode.submit(makeAlarm("R1", QStringLiteral("data/alarms/20261029/ch1_rel.jpg")));
        relMode.pumpOut();
        const QString relUrl =
            relPeer.dataAt(0)[QStringLiteral("snapshot")].toObject()[QStringLiteral("url")]
                .toString();
        check(relUrl == base + QStringLiteral("/20261029/ch1_rel.jpg"),
              "相对 imagePath 必须先按 cwd 绝对化再换算，否则根目录会被拼进地址拼成 404");
        check(!relUrl.contains(QStringLiteral("/data/alarms/")),
              "地址是相对服务根目录的，根目录自己不该出现在路径里");

        // ---- 14) 第二段：media_begin(transfer:"http")，且没有 media_end ----
        AlarmUplinkEngine urlMedia;
        FakeTransport urlMediaPeer;
        urlMedia.setTransport(&urlMediaPeer);
        urlMedia.setSettings(urlSettings);

        uplink::MediaJob mp4Job;
        mp4Job.mediaId = QStringLiteral("c-url0001");
        mp4Job.alarmId = QStringLiteral("U1");
        mp4Job.kind = QString::fromLatin1(uplink::kKindClip);
        mp4Job.name = QStringLiteral("ch1_un_helmet_101112_345.mp4");
        mp4Job.mime = QString::fromLatin1(uplink::kMimeMp4);
        const QString mp4Url = base + QStringLiteral("/20261029/ch1_un_helmet_101112_345.mp4");
        const QString sha = QStringLiteral("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        check(urlMedia.submitMediaUrl(mp4Job, QString(), 1234567, sha).isEmpty(),
              "空地址不产生媒体项");
        check(!urlMedia.submitMediaUrl(mp4Job, mp4Url, 1234567, sha).isEmpty(),
              "submitMediaUrl 回带 media_id");
        check(urlMedia.pendingCount() == 1, "一段地址 = 一个待发项");
        check(urlMedia.pumpOut() == 1, "单个文本帧一次泵完");
        check(urlMediaPeer.textCount() == 1 && urlMediaPeer.binaryCount() == 0,
              "url 模式没有任何二进制帧：字节不经过这条链路");
        check(urlMediaPeer.countType(uplink::kTypeMediaBegin) == 1, "仍复用 media_begin 这个类型名");
        check(urlMediaPeer.countType(uplink::kTypeMediaEnd) == 0,
              "url 模式不发 media_end：没有帧序列要收尾（项被标成 media=true 就会凭空造一条）");

        const QJsonObject bData = urlMediaPeer.dataAt(0);
        check(bData["media_id"].toString() == mp4Job.mediaId, "要回指 alarm_info 里承诺的那个 id");
        check(bData["transfer"].toString() == QLatin1String(uplink::kTransferHttp), "transfer=http");
        check(bData["url"].toString() == mp4Url, "url 原样带上");
        check(bData["mime"].toString() == QLatin1String(uplink::kMimeMp4), "片段容器按真实后缀报");
        check(bData["total_bytes"].toDouble() == 1234567.0, "total_bytes 是写稳后采到的真实长度");
        check(bData["sha256"].toString() == sha, "哈希保留：对端下载完能自校验");
        check(!bData.contains("chunk_size") && !bData.contains("total_chunks"),
              "分块字段在 http 那档没有意义，不该出现");
        // 哈希算不出来就整个省略，而不是写一个空串让对端以为校验过了
        check(!urlMedia.submitMediaUrl(mp4Job, mp4Url, 10, QString()).isEmpty(),
              "哈希缺席不影响这段地址可发");
        urlMedia.pumpOut();
        check(!urlMediaPeer.dataAt(1).contains("sha256"), "算不出的哈希要省略而不是给空串");

        // ---- 15) 重连不得把已经申报的地址再报一遍 --------------------------
        // 地址项标成 media=true 的话，restartInFlightMedia() 会把它的游标清零，
        // 每次重连对端就多收到一条 media_begin（然后按 sha256 判"重复"去重）。
        // 用 expect_ack 把项留在队首，才能把这条不变量看出来
        AlarmUplinkEngine acked;
        FakeTransport ackedPeer;
        acked.setTransport(&ackedPeer);
        AlarmUplinkEngine::Settings ackSettings = urlSettings;
        ackSettings.expectAck = true;
        ackSettings.ackTimeoutMs = 600000;   // 远到本轮不可能超时
        acked.setSettings(ackSettings);
        check(!acked.submitMediaUrl(mp4Job, mp4Url, 1234567, sha).isEmpty(),
              "入队项应有 media_id 回带");
        check(acked.pumpOut() == 0 && ackedPeer.textCount() == 1,
              "等确认的项发完帧仍留在队首，不算发完");
        check(acked.hasPending(), "没收到 uplink_ack 之前不能出队");
        acked.restartInFlightMedia();
        check(acked.pumpOut() == 0, "等确认超时之前不许再发");
        check(ackedPeer.countType(uplink::kTypeMediaBegin) == 1,
              "重连不该把同一段的地址再申报一遍（只有分块项才需要整段重发）");

        // 队满丢弃时，已申报的媒体项被挤掉要在日志里说清楚（否则对端手里是空头承诺）
        AlarmUplinkEngine evict;
        FakeTransport evictPeer;
        evictPeer.connected = false;
        QVector<QPair<QString, QString>> evictLogs;
        evict.setTransport(&evictPeer);
        evict.setLogger([&evictLogs](const QString &cat, const QString &msg) {
            evictLogs.append(qMakePair(cat, msg));
        });
        AlarmUplinkEngine::Settings tinyQueue = urlSettings;
        tinyQueue.maxPending = 1;
        evict.setSettings(tinyQueue);
        check(!evict.submitMediaUrl(mp4Job, mp4Url, 1234567, sha).isEmpty(),
              "第一条地址应入队");
        uplink::MediaJob later = mp4Job;
        later.mediaId = QStringLiteral("c-url0002");
        check(!evict.submitMediaUrl(later, base + QStringLiteral("/20261029/later.mp4"), 20, sha).isEmpty(),
              "第二条地址也应入队（挤掉第一条而不是把自己丢掉）");
        bool loud = false;
        for (const auto &entry : evictLogs) {
            if (entry.first == QLatin1String("error")
                && entry.second.contains(QStringLiteral("对端会拿不到"))) {
                loud = true;
            }
        }
        check(loud, "挤掉已申报的媒体地址要有一条 error：本机日志里看不出来就等于没人知道");

        // ---- 16) media_failed：承诺落空要说给对端 --------------------------
        AlarmUplinkEngine failed;
        FakeTransport failedPeer;
        failed.setTransport(&failedPeer);
        failed.setSettings(urlSettings);
        failed.submitMediaFailed(QStringLiteral("c-gone"), QStringLiteral("U1"),
                                 QString::fromLatin1(uplink::kKindClip),
                                 QStringLiteral("mp4 副本没写成"));
        failed.submitMediaFailed(QString(), QStringLiteral("U1"),
                                 QString::fromLatin1(uplink::kKindClip),
                                 QStringLiteral("没承诺过"));
        check(failed.pendingCount() == 1, "没有 media_id 的不该入队（没承诺就无所谓落空）");
        check(failed.pumpOut() == 1, "一条 media_failed = 一个文本帧");
        check(failedPeer.jsonAt(0)["type"].toString() == QLatin1String(uplink::kTypeMediaFailed),
              "类型名集中在一处，改名只动这里");
        const QJsonObject fData = failedPeer.dataAt(0);
        check(fData["media_id"].toString() == QStringLiteral("c-gone"), "要指回那条 planned");
        check(fData["kind"].toString() == QLatin1String(uplink::kKindClip), "kind 让对端知道撤哪一条");
        check(fData["reason"].toString() == QStringLiteral("mp4 副本没写成"), "原因用人能读的话");
        check(!fData.contains("url") && !fData.contains("total_bytes"),
              "落空的媒体没有任何可交付内容，别留看起来像有字段的字段");
    }

    // ---- 17) mediaUrlFor：前缀拼接、越界不给、百分号编码 -------------------
    {
        const QString root = QStringLiteral("/data/alarms");
        const QString base = QStringLiteral("http://10.0.0.5:8090");
        check(uplink::mediaUrlFor(base, root, root + QStringLiteral("/20261029/a.jpg"))
                  == base + QStringLiteral("/20261029/a.jpg"),
              "根目录之下的文件应拼成 前缀/相对路径");
        check(uplink::mediaUrlFor(base + QStringLiteral("/"), root, root + QStringLiteral("/a.jpg"))
                  == base + QStringLiteral("/a.jpg"),
              "前缀带尾斜杠也只拼一个斜杠（配置里写不写都行）");
        check(uplink::mediaUrlFor(QString(), root, root + QStringLiteral("/a.jpg")).isEmpty(),
              "没有前缀就没有地址，不能只给个相对路径");
        check(uplink::mediaUrlFor(base, QString(), root + QStringLiteral("/a.jpg")).isEmpty(),
              "没有根目录就无从判断相对谁");
        check(uplink::mediaUrlFor(base, root, QString()).isEmpty(), "空路径不给地址");
        check(uplink::mediaUrlFor(base, root, QStringLiteral("/etc/passwd")).isEmpty(),
              "root 之外的绝对路径一律不给：服务零鉴权，拼出去等于公开本机任意可读文件");
        check(uplink::mediaUrlFor(base, root, root + QStringLiteral("/../../etc/passwd")).isEmpty(),
              "带 .. 的路径要算成越界，而不是原样拼进地址里赌服务器会拦");
        check(uplink::mediaUrlFor(base, root, root).isEmpty(),
              "根目录本身不是一个文件（relativeFilePath 回一个点），别拼出一个可点开的目录");
        // 中文类别名是 labels.txt 里的合法取值，裸拼进去对端拿到的是非法 URL
        const QString cn = uplink::mediaUrlFor(base, root, root + QStringLiteral("/20261029/ch1_人员_1.jpg"));
        check(!cn.isEmpty() && !cn.contains(QStringLiteral("人员")),
              "非 ASCII 的路径段必须百分号编码");
        check(cn.contains(QStringLiteral("%E4%BA%BA")), "按 UTF-8 编码，人 = %E4%BA%BA");
    }

    // ---- 18) 退避梯子与 uplink_ack 对账 ------------------------------------
    {
        // 梯子本身：指数翻到封顶就不许再翻（翻过头一次等几分钟，队列等于死锁）
        check(uplink::backoffMs(0, uplink::kRetryBaseMs) == 0,
              "第 0 次不给等待：超时那一下已经等过一整个 ack_timeout 了");
        check(uplink::backoffMs(1, uplink::kRetryBaseMs) == 2000, "首次重试等 2s");
        check(uplink::backoffMs(2, uplink::kRetryBaseMs) == 4000, "第二次 4s");
        check(uplink::backoffMs(3, uplink::kRetryBaseMs) == 8000, "第三次 8s");
        check(uplink::backoffMs(4, uplink::kRetryBaseMs) == 16000, "第四次 16s");
        check(uplink::backoffMs(5, uplink::kRetryBaseMs) == uplink::kBackoffCapMs,
              "第五次该被 30s 帽住（2^5×2s=64s 不许原样返回）");
        check(uplink::backoffMs(60, uplink::kRetryBaseMs) == uplink::kBackoffCapMs,
              "次数再大也不该溢出成负数或天文数字");
        check(uplink::backoffMs(-1, uplink::kRetryBaseMs) == 0, "负数按不等待处理");

        AlarmUplinkEngine ack;
        FakeTransport ackPeer;
        QVector<QPair<QString, QString>> ackLogs;
        ack.setTransport(&ackPeer);
        ack.setLogger([&ackLogs](const QString &cat, const QString &msg) {
            ackLogs.append(qMakePair(cat, msg));
        });
        AlarmUplinkEngine::Settings ackSettings;
        ackSettings.expectAck = true;
        ackSettings.ackTimeoutMs = 600000;   // 长到本轮不可能自己超时
        ack.setSettings(ackSettings);

        const uplink::SubmitResult r = ack.submit(makeAlarm("ACK1", "/p/ack.jpg"));
        ack.pumpOut();
        check(ack.pendingCount() == 1 && ackPeer.textCount() == 1,
              "开关打开时发完帧要留在队首等确认，不许自己判完成");
        check(ack.nextWaitMs() > uplink::kFlushWaitMs,
              "等确认期间调用方该等到超时点，而不是每 20ms 醒一次");

        // 对端把 id 写错：只能记一条 warning，不能顺手把队首放走
        ack.handleAck(QStringLiteral("a-wrong"), QString(),
                      QLatin1String("info"), QLatin1String("ok"), QString());
        check(ack.pendingCount() == 1, "id 对不上队首时不能认领任何东西");
        bool mismatchLogged = false;
        for (const auto &entry : ackLogs) {
            if (entry.first == QLatin1String("warning")
                && entry.second.contains(QStringLiteral("对不上"))) {
                mismatchLogged = true;
            }
        }
        check(mismatchLogged, "对不上必须留一条日志：对端实现写错时这是唯一线索");

        // ok：出队，这一项到此走完
        ack.handleAck(r.messageId, QString(), QLatin1String("info"), QLatin1String("ok"), QString());
        check(ack.pendingCount() == 0, "确认 ok 才算这一项走完");

        // retry：整项重发。首次重试不额外等待（等确认那一段本身已经耗掉一个 ack_timeout）
        const uplink::SubmitResult r2 = ack.submit(makeAlarm("ACK2", "/p/ack2.jpg"));
        ack.pumpOut();
        ack.handleAck(r2.messageId, QString(), QLatin1String("info"), QLatin1String("retry"),
                      QStringLiteral("JSON 少字段"));
        check(ack.pendingCount() == 1, "要求重发的项还在");
        ack.pumpOut();
        // 计数是整条链路累计的：ACK1 一次 + ACK2 首末两次 = 3
        check(ackPeer.countType(uplink::kTypeAlarmInfo) == 3,
              "retry 之后整项重发，同一条 alarm_info 出现两次");

        // 第二次 retry 才进退避梯子：attempts=2 → backoffMs(1)=2s，没到点一帧都不许发
        ack.handleAck(r2.messageId, QString(), QLatin1String("info"), QLatin1String("retry"),
                      QStringLiteral("还是不对"));
        check(ack.pumpOut() == 0 && ackPeer.countType(uplink::kTypeAlarmInfo) == 3,
              "退避中的项必须安静等到那个点");
        check(ack.nextWaitMs() > uplink::kFlushWaitMs, "调用方该按 nextWaitMs 挂定时器，别 20ms 空转");
        // 退避中 awaitingAck 是 false：此时来的确认一律不认（连自己上一次发到哪都还没定，
        // 认了就是把别轮的确认算到这一项头上）
        ack.handleAck(r2.messageId, QString(), QLatin1String("info"), QLatin1String("ok"), QString());
        check(ack.pendingCount() == 1, "退避中的项不认确认");

        // reject：不重发，直接丢（重发只会再来一次 reject）
        ack.clear();
        const uplink::SubmitResult r3 = ack.submit(makeAlarm("ACK3", "/p/ack3.jpg"));
        ack.pumpOut();
        ack.handleAck(r3.messageId, QString(), QLatin1String("info"), QLatin1String("reject"),
                      QStringLiteral("通道不在订阅列表"));
        check(ack.pendingCount() == 0, "reject 的项直接丢弃，不进退避循环");
        // ACK1 一次 + ACK2 两次（retry 重发）+ ACK3 一次 = 4，reject 没有贡献第五次
        check(ackPeer.countType(uplink::kTypeAlarmInfo) == 4, "reject 不该重发");

        // 迟到的确认（那一项早就超时丢了）：必须安静，不能崩也不能把队首新项认走
        ack.submit(makeAlarm("ACK4", "/p/ack4.jpg"));
        ack.handleAck(r3.messageId, QString(), QLatin1String("info"), QLatin1String("ok"), QString());
        check(ack.pendingCount() == 1, "旧 id 的迟到确认不许把当前队首放走");
    }

    std::cout << "alarm_info shape / coalesce / queue bound / watermark / switches / slot cap"
                 " / chunk plan / media frames / full-segment resend / disk read"
                 " / clip promise / url transport / ack & backoff contracts passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
}
