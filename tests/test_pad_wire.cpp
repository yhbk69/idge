// ============================================================================
// test_pad_wire.cpp — AlarmRecord → Pad 接口 v1 JSON 的离线自检
// ----------------------------------------------------------------------------
// 只链 Qt5::Core + pad_wire.cpp + alarm_uplink_engine.cpp（借它的 mediaUrlFor）：
// 不起 WS 服务端、不开数据库、不碰 GUI，所以能在板端之外跑。
//
// 为什么值得单独钉：这份映射是**协议边界**，对端按 alarmId+updatedAt 合并、按 taskId 过滤、
// 按 evidence URL 取图。这几条写错在板端肉眼是看不出来的——
//   · updatedAt 不带时区 → 对端把游标算歪，重连时漏掉整批报警；
//   · rectified 映射成 DISPOSED → "我看过了"被上报成"我处理过了"，处置闭环数据全脏；
//   · taskId 取错键 → 对端按施工计划过滤，一条也显示不出来；
//   · 越界的媒体路径被拼进 URL → 零鉴权服务上等于任意文件可读。
//
// 覆盖十五条契约：
//   1) wireTime 补本机时区偏移，且对已带偏移/带 Z 的输入幂等
//   2) wireTime 对空串与坏文本回落到"现在"（updatedAt 不能为空）
//   3) internalTimeFromWire 与 wireTime 往返一致；认不出的形状返回空串（= 按全量处理）
//   4) wireStatus：pending/rectified/未知 → PENDING，false_alarm → FALSE_ALARM
//   5) wireReadAt：read_time 优先；只有 rectified 时用 update_time 兜底；未读返回空串
//   6) taskIdOf 取 construction_info 的 construction_id；空/坏 JSON 返回空串
//   7) typeOf：围栏按 alarmType（fence_in→INTRUSION / fence_out→CROSS），检测按类别名表
//   8) typeOf 兜底：认不出的类别抬成大写 code + "其他"，绝不给空标签
//   9) alarmJson 必填键齐全，level 是数字，occurredAt/updatedAt 带偏移
//  10) alarmJson 的 readAt 未读时是 null（不是缺键也不是空串）
//  11) evidence.snapshotUrls 只 1 个元素，且 URL = baseUrl + root 内相对路径
//  12) 证据路径在 root 之外 → snapshotUrls 为空数组（不拼越界地址）
//  13) 片段 .mp4 不存在 → 不写 clipUrl（宁缺勿假）；存在 → 写出来
//  14) alarmUpdateJson 不含 evidence，updatedAt 必填，dispose 只在有处置文本时出现
//  15) 信封：alarm/sync_result 带 code=0+message=ok；pong 不带 code；error 的 code 在信封层
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_wire，rc 0=PASS 1=FAIL
// ============================================================================

#include "pad_wire.h"

#include <QByteArray>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTime>
#include <QDateTime>

#include <iostream>
#include <stdexcept>



static void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

// 一条"现场最常见形状"的报警：通道 0 检出未戴安全帽，级别 3，带施工信息。
static AlarmRecord makeRecord()
{
    AlarmRecord r;
    r.id = QStringLiteral("0f8fad5bd9cb469fa16570867728950e");
    r.alarmType = QStringLiteral("detection");
    r.alarmLevel = 3;
    r.alarmTime = QStringLiteral("2026-11-02T17:30:00");
    r.channel = 0;
    r.clsId = 0;
    r.className = QStringLiteral("un_helmet");
    r.confidence = 0.964f;
    r.status = QStringLiteral("pending");
    r.createTime = r.alarmTime;
    r.updateTime = r.alarmTime;
    r.constructionJson = QStringLiteral(
        "{\"construction_id\":\"BJ5-20261102-023\",\"worker_name\":\"张伟\"}");
    return r;
}

// 期望的偏移文本（本机时区算出来是什么就断言什么，不在测试里写死 +08:00）
static QString expectedOffset()
{
    const QDateTime local(QDate(2026, 11, 2), QTime(17, 30, 0), Qt::LocalTime);
    const QDateTime asUtc(QDate(2026, 11, 2), QTime(17, 30, 0), Qt::UTC);
    const int seconds = int((asUtc.toMSecsSinceEpoch() - local.toMSecsSinceEpoch()) / 1000);
    const int sign = seconds < 0 ? -1 : 1;
    const int abs = qAbs(seconds);
    return QStringLiteral("%1%2:%3")
        .arg(sign < 0 ? QLatin1Char('-') : QLatin1Char('+'))
        .arg(abs / 3600, 2, 10, QLatin1Char('0'))
        .arg((abs % 3600) / 60, 2, 10, QLatin1Char('0'));
}

int main()
{
try {
    const QString offset = expectedOffset();

    // ── 1/2：出门口补偏移，且幂等；坏输入不返回空 ────────────────────────────
    {
        check(pad::wireTime(QStringLiteral("2026-11-02T17:30:00"))
                  == QStringLiteral("2026-11-02T17:30:00") + offset,
              "wireTime 应把本机墙上时间补上本机时区偏移");
        const QString already = QStringLiteral("2026-11-02T17:30:00+08:00");
        check(pad::wireTime(already) == already, "wireTime 对已带偏移的输入必须幂等");
        check(pad::wireTime(QStringLiteral("2026-11-02T09:30:00Z"))
                  == QStringLiteral("2026-11-02T09:30:00Z"),
              "wireTime 对 Z 结尾同样幂等");
        const QString fallback = pad::wireTime(QString());
        check(!fallback.isEmpty() && fallback.contains(QLatin1Char('+')),
              "空串/坏文本要回落成“现在”，updatedAt 不能是空");
        check(!pad::wireTime(QStringLiteral("不是时间")).isEmpty(),
              "解析失败也要给可用文本");
    }

    // ── 3：游标反向换算与往返 ───────────────────────────────────────────────
    {
        const QString internal = QStringLiteral("2026-11-02T23:47:26");
        const QString wire = pad::wireTime(internal);
        check(pad::internalTimeFromWire(wire) == internal,
              "对端游标换算回库内文本必须与原值一致（否则增量同步会错位）");
        check(pad::internalTimeFromWire(QString()).isEmpty(), "空游标 = 全量");
        check(pad::internalTimeFromWire(QStringLiteral("garbage")).isEmpty(),
              "认不出的游标返回空串，由调用方按全量处理");
        // 对端给了别的时区：换算回本机墙上时间，不能截前 19 字符
        const QString other = QStringLiteral("2026-11-02T23:47:26+00:00");
        const QString converted = pad::internalTimeFromWire(other);
        check(converted.size() == 19 && converted != other.left(19),
              "非本机时区的游标要按时区换算，不是截断");
    }

    // ── 4：状态三值 ─────────────────────────────────────────────────────────
    {
        AlarmRecord r = makeRecord();
        r.status = QStringLiteral("pending");
        check(pad::wireStatus(r) == QLatin1String("PENDING"), "pending → PENDING");
        r.status = QStringLiteral("rectified");
        check(pad::wireStatus(r) == QLatin1String("PENDING"),
              "rectified 是“已确认/已读”，不许映射成 DISPOSED");
        r.status = QStringLiteral("false_alarm");
        check(pad::wireStatus(r) == QLatin1String("FALSE_ALARM"), "false_alarm → FALSE_ALARM");
        r.status = QStringLiteral("whatever");
        check(pad::wireStatus(r) == QLatin1String("PENDING"), "认不出的状态按 PENDING（v1 §3.10）");
    }

    // ── 5：readAt 的三级回落 ───────────────────────────────────────────────
    {
        AlarmRecord r = makeRecord();
        check(pad::wireReadAt(r).isEmpty(), "未读必须返回空串，不能编一个时间");
        r.readTime = QStringLiteral("2026-11-02T18:00:00");
        check(pad::wireReadAt(r) == QStringLiteral("2026-11-02T18:00:00") + offset,
              "read_time 有值时原样带出");
        r.readTime = QString();
        r.status = QStringLiteral("rectified");
        r.updateTime = QStringLiteral("2026-11-02T18:05:00");
        check(pad::wireReadAt(r) == QStringLiteral("2026-11-02T18:05:00") + offset,
              "只有确认动作时，已读时间取状态变更那一刻");
    }

    // ── 6：taskId 只有一个来源 ─────────────────────────────────────────────
    {
        check(pad::taskIdOf(makeRecord()) == QStringLiteral("BJ5-20261102-023"),
              "taskId 取 construction_info.construction_id");
        AlarmRecord r = makeRecord();
        r.constructionJson = QString();
        check(pad::taskIdOf(r).isEmpty(), "未登录的报警没有归属，返回空串");
        r.constructionJson = QStringLiteral("{坏 JSON");
        check(pad::taskIdOf(r).isEmpty(), "坏 JSON 不异常，按空处理");
    }

    // ── 7/8：类型三元组 ────────────────────────────────────────────────────
    {
        AlarmRecord r = makeRecord();
        const pad::TypeWire t = pad::typeOf(r);
        check(t.code == QLatin1String("HELMET") && t.typeLabel == QStringLiteral("未戴安全帽")
                  && t.categoryLabel == QStringLiteral("PPE违规"),
              "un_helmet → HELMET/未戴安全帽/PPE违规（v1 §3.6 示例的同一套词）");

        r.alarmType = QStringLiteral("fence_in");
        check(pad::typeOf(r).code == QLatin1String("INTRUSION"), "fence_in → INTRUSION");
        r.alarmType = QStringLiteral("fence_out");
        check(pad::typeOf(r).code == QLatin1String("CROSS"), "fence_out → CROSS");

        r.alarmType = QStringLiteral("detection");
        r.className = QStringLiteral("cigarette");
        check(pad::typeOf(r).code == QLatin1String("SMOKING"), "cigarette → SMOKING");

        r.className = QStringLiteral("whatever");
        const pad::TypeWire fb = pad::typeOf(r);
        check(fb.code == QLatin1String("WHATEVER") && !fb.typeLabel.isEmpty()
                  && !fb.categoryLabel.isEmpty(),
              "认不出的类别也要给 code 与标签（Pad 不做本地映射）");

        r.className = QString();
        check(pad::typeOf(r).code == QLatin1String("UNKNOWN"), "空类别名兜底成 UNKNOWN");
    }

    // ── 9/10：完整快照的必填项 ─────────────────────────────────────────────
    {
        const QJsonObject data = pad::alarmJson(makeRecord(), QString(), QString());
        static const char *kRequired[] = {"alarmId", "taskId", "level",    "type",
                                          "typeLabel", "categoryLabel", "status",
                                          "occurredAt", "updatedAt", "confidence", "evidence"};
        for (const char *key : kRequired) {
            check(data.contains(QLatin1String(key)), "alarm 快照缺必填键");
        }
        check(data.value(QLatin1String("level")).isDouble(), "level 必须是数字（v1 是 int 1~4）");
        check(data.value(QLatin1String("occurredAt")).toString().endsWith(offset),
              "occurredAt 要带时区偏移");
        check(data.value(QLatin1String("updatedAt")).toString().endsWith(offset),
              "updatedAt 要带时区偏移");
        check(data.value(QLatin1String("readAt")).isNull(),
              "未读时 readAt 是 null，而不是缺键或空串");
        check(!data.contains(QLatin1String("title")) && !data.contains(QLatin1String("cameraId")),
              "v1 §3.6 已移除的字段不许再出现");
    }

    // ── 11/12/13：证据 URL ─────────────────────────────────────────────────
    {
        QTemporaryDir dir;
        check(dir.isValid(), "临时目录建不出来，无法验证证据 URL");
        const QString root = QDir(dir.path()).absolutePath();
        const QString day = root + QStringLiteral("/20261102");
        QDir().mkpath(day);
        const QString snap = day + QStringLiteral("/ch1_un_helmet_173000_045.jpg");
        {
            QFile f(snap);
            check(f.open(QIODevice::WriteOnly), "抓拍测试文件写不出来");
            f.write("x");
        }

        AlarmRecord r = makeRecord();
        r.imagePath = snap;
        const QString base = QStringLiteral("http://192.168.124.29:8090");
        QJsonObject evidence = pad::evidenceOf(r, base, root);
        const QJsonArray snaps = evidence.value(QLatin1String("snapshotUrls")).toArray();
        check(snaps.size() == 1, "v1 §3.6：snapshotUrls 只放 1 个元素");
        check(snaps.at(0).toString()
                  == base + QStringLiteral("/20261102/ch1_un_helmet_173000_045.jpg"),
              "URL = baseUrl + root 内的相对路径");
        check(!evidence.contains(QLatin1String("clipUrl")),
              "片段还没落盘时不写 clipUrl（预告了取不到比对端更坏）");

        r.videoPath = day + QStringLiteral("/ch1_un_helmet_173000_045.h265");
        evidence = pad::evidenceOf(r, base, root);
        check(!evidence.contains(QLatin1String("clipUrl")),
              "同名 .mp4 不存在时依旧不写 clipUrl");
        {
            QFile mp4(day + QStringLiteral("/ch1_un_helmet_173000_045.mp4"));
            check(mp4.open(QIODevice::WriteOnly), "mp4 测试文件写不出来");
            mp4.write("x");
        }
        evidence = pad::evidenceOf(r, base, root);
        check(evidence.value(QLatin1String("clipUrl")).toString()
                  == base + QStringLiteral("/20261102/ch1_un_helmet_173000_045.mp4"),
              "mp4 到位后 clipUrl 指向它");

        // 越界：root 之外的路径一个字节都不许给出去（服务是零鉴权的）
        AlarmRecord outside = makeRecord();
        outside.imagePath = QStringLiteral("/etc/hostname");
        const QJsonArray none =
            pad::evidenceOf(outside, base, root)
                .value(QLatin1String("snapshotUrls")).toArray();
        check(none.isEmpty(), "证据路径越界必须返回空数组，不拼地址");

        // 媒体服务没起来（baseUrl 空）→ 只给空数组，不臆造地址
        const QJsonArray noBase =
            pad::evidenceOf(r, QString(), root)
                .value(QLatin1String("snapshotUrls")).toArray();
        check(noBase.isEmpty(), "没有 baseUrl 时不给证据地址");
    }

    // ── 14：alarm_update 的取舍 ────────────────────────────────────────────
    {
        AlarmRecord r = makeRecord();
        r.status = QStringLiteral("false_alarm");
        r.updateTime = QStringLiteral("2026-11-02T18:20:00");
        const QJsonObject data = pad::alarmUpdateJson(r);
        check(!data.contains(QLatin1String("evidence")),
              "v1 §13.2.3 的 alarm_update 不带 evidence");
        check(data.value(QLatin1String("status")).toString() == QLatin1String("FALSE_ALARM"),
              "update 里的状态同样走三值映射");
        check(data.value(QLatin1String("updatedAt")).toString()
                  == QStringLiteral("2026-11-02T18:20:00") + offset,
              "updatedAt 必填且反映本次变更（§3.7）");
        check(!data.contains(QLatin1String("dispose")), "没有处置文本就不该有 dispose 对象");

        r.disposeResult = QStringLiteral("已现场纠正");
        r.disposeUserName = QStringLiteral("张伟");
        r.disposeTime = QStringLiteral("2026-11-02T18:20:00");
        const QJsonObject dispose =
            pad::alarmUpdateJson(r).value(QLatin1String("dispose")).toObject();
        check(dispose.value(QLatin1String("measure")).toString() == QStringLiteral("已现场纠正")
                  && dispose.contains(QLatin1String("disposedAt")),
              "处置记录带 measure 与 disposedAt（§13.2.4）");
    }

    // ── 15：信封形状 ──────────────────────────────────────────────────────
    {
        const QJsonObject data;
        const QJsonObject alarm = QJsonDocument::fromJson(
            pad::envelope(QStringLiteral("alarm"),
                          pad::alarmJson(makeRecord(), QString(), QString()))).object();
        check(alarm.value(QLatin1String("type")).toString() == QLatin1String("alarm")
                  && alarm.value(QLatin1String("code")).toInt() == 0
                  && alarm.value(QLatin1String("message")).toString() == QLatin1String("ok")
                  && alarm.contains(QLatin1String("ts")) && alarm.contains(QLatin1String("data")),
              "普通消息的信封是 type/ts/code/message/data");
        check(!alarm.contains(QLatin1String("v")) && !alarm.contains(QLatin1String("requestId")),
              "v1 §3.2：信封没有 v、没有 requestId");

        const QJsonObject pong =
            QJsonDocument::fromJson(pad::pongEnvelope()).object();
        check(pong.value(QLatin1String("type")).toString() == QLatin1String("pong")
                  && !pong.contains(QLatin1String("code")),
              "pong 只带 type/ts");

        const QJsonObject err = QJsonDocument::fromJson(
            pad::errorEnvelope(2001, QStringLiteral("未知消息类型"),
                               QStringLiteral("type=foo"))).object();
        check(err.value(QLatin1String("code")).toInt() == 2001
                  && err.value(QLatin1String("message")).toString() == QStringLiteral("未知消息类型")
                  && err.value(QLatin1String("data")).toObject()
                         .value(QLatin1String("detail")).toString() == QStringLiteral("type=foo"),
              "error 的 code/message 在信封层，data 只放 detail（§13.1.8）");

        const QJsonObject ack1001 = QJsonDocument::fromJson(
            pad::envelopeWithCode(QStringLiteral("hello_ack"), 1001,
                                  QStringLiteral("协议版本不兼容"), QJsonObject())).object();
        check(ack1001.value(QLatin1String("code")).toInt() == 1001,
              "协议版本不兼容时 hello_ack 带 code=1001（§3.5）");
    }

    std::cout << "pad wire contracts passed: timezone round-trip / status 3-values / readAt "
                 "fallback / taskId from construction_id / type table + fallback / required keys "
                 "/ evidence URL + traversal refusal + clip presence / alarm_update shape "
                 "/ envelope without v & requestId\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
}
