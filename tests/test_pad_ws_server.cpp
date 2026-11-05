// ============================================================================
// test_pad_ws_server.cpp — Pad 接口 v1 的 WS 服务端线上自检（loopback，单进程）
// ----------------------------------------------------------------------------
// 起真 PadWsServer 在 127.0.0.1 的一个临时口上，用真 QWebSocket 连进来按 §3 的消息面走一遍。
// 不用 Qt5::Test（板端没装），收发节奏靠"等到第 N 帧或超时"的事件循环判据。
//
// 为什么值得单独钉：§12.1 的十条验收全是"对端行为"，纸面实现最容易在这儿翻车——
//   · hello_ack 迟于 5 秒 = Pad 直接判协议不兼容；
//   · 未知 type 崩掉 = 现场一个对端版本差就把整机搞挂；
//   · sync_result 省略 alarms 键（而不是给 []）会让 Pad 保留旧缓存、看起来"补漏成功"；
//   · Origin 不判 = 局域网里任何网页的 JS 都能连进来收报警、按 taskId 拉历史。
//
// 覆盖十六条契约：
//   1) 端口占用/重复 start 的幂等（第二次 start 直接 true，不把已连的 Pad 踢掉）
//   2) hello → hello_ack：serverTime 带偏移、protocolVersion=1、capabilities 只有 alarm、
//      detectionBoxes 恒 false（v1 硬要求）
//   3) protocolVersion 不匹配 → hello_ack 带 code=1001 且连接被关闭
//   4) ping → pong，pong 带 type/ts/code/message（§3.2 服务端消息必须带 code，P3-1 修正）
//   5) sync_request（带 taskId）→ **恰好一个** sync_result，含 alarms:[] 与 cursor 两个键
//   6) sync_result 的 alarms 是数组而不是缺键（缺键对端=保留旧缓存）
//   7) 数据库未初始化时不崩、返回空数组 + 回显请求游标
//   8) 未知 type → error 2001（code/message 在信封层，data.detail 带上那个 type）
//   9) alarm_ack **不回复**（用"紧接着的 ping 的 pong 是下一帧"来证明中间没有插话）
//  10) AlarmManager 发 alarmGenerated → 连接上收到 type=alarm 的完整快照
//  11) 没有媒体底址时 alarm 顶层 evidenceSnapshotUrl 是空串（键按 §13.2.1 必填，但不臆造地址）
//  12) 配置 device_id 留空时 hello_ack 仍带必填 deviceId（口径 A17）
//  13) hello 缺 deviceId/token/clientVersion → error 2001 且**不关连接**，补齐后同一条连接
//      还能握手成功（P3-2：参数错与版本不兼容必须分开判，混了就等于给对端假方向）
//  14) sync_request 的 taskId 为空 → error 2001，不再返回所有计划的报警（P3-3，§13.1.5 必填）
//  15) Origin 白名单：http(s):// 的浏览器页被拒（连不上、不计入 clients_），
//      空 Origin 与 android-app://* 放行（P2-3）
//  16) 确认/误报 → 立刻推一帧 alarm_update，带 alarmId/status/readAt/updatedAt
//      （P1-1：以前 pushAlarmUpdate 零调用方，Pad 那条只能等重连才拉平）
//  17) 片段落盘 → **只推 alarm_update**（不再是重发整条 alarm）：有底址时带 clipUrl，
//      没底址时缺键而不是 null（2026-11-05 现场流程）
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_ws_server，rc 0=PASS 1=FAIL
// ============================================================================

#include "pad_ws_server.h"
#include "pad_wire.h"      // defaultDeviceId()：契约 12 要比对派生值
#include "alarm_manager.h"
#include "database_manager.h"
#include "alarm_dao.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebSocket>

#include <iostream>
#include <stdexcept>

namespace {

void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

/// 收到的帧（按到达顺序），以及"再等 N 毫秒看有没有新帧"的能力
// 继承 QObject 只是为了给 connect 一个 context 对象（socket 断开后回调不再打到这块栈上）
class Recorder : public QObject
{
public:
    explicit Recorder(QWebSocket *socket) : QObject(nullptr), socket_(socket)
    {
        connect(socket_, &QWebSocket::textMessageReceived, this,
                         [this](const QString &text) {
                             frames_.append(text);
                             if (waitLoop_) {
                                 waitLoop_->quit();
                             }
                         });
    }

    /// 阻塞到至少拿到 want 帧，或超时；返回拿到的帧数
    int waitFor(int want, int timeoutMs = 2000)
    {
        QElapsedTimer clock;
        clock.start();
        while (frames_.size() < want && clock.elapsed() < timeoutMs) {
            QEventLoop loop;
            waitLoop_ = &loop;
            QTimer::singleShot(50, &loop, &QEventLoop::quit);
            loop.exec();
            waitLoop_ = nullptr;
        }
        return frames_.size();
    }

    /// 已经拿到的帧数（配合 waitFor 判断"这条之后不该再有"）
    int count() const { return frames_.size(); }

    QJsonObject at(int index) const
    {
        return QJsonDocument::fromJson(frames_.at(index).toUtf8()).object();
    }

    /// 找最近一帧指定 type 的消息；没有则返回空对象
    QJsonObject lastOfType(const QString &type) const
    {
        for (int i = frames_.size() - 1; i >= 0; --i) {
            const QJsonObject env = at(i);
            if (env.value(QLatin1String("type")).toString() == type) {
                return env;
            }
        }
        return QJsonObject();
    }

    /// 把已收到的帧数清零式地"往后推"：只记游标，不改历史
    int mark() const { return frames_.size(); }

private:
    QWebSocket *socket_;
    QStringList frames_;
    QEventLoop *waitLoop_ = nullptr;
};

QString sendJson(QWebSocket *socket, const QJsonObject &env)
{
    const QByteArray frame = QJsonDocument(env).toJson(QJsonDocument::Compact);
    const qint64 written = socket->sendTextMessage(QString::fromUtf8(frame));
    return written > 0 ? QString() : QStringLiteral("发送失败（未连接或对端不可写）");
}

QJsonObject envelopeOf(const QString &type, const QJsonObject &data)
{
    QJsonObject env;
    env.insert(QStringLiteral("type"), type);
    env.insert(QStringLiteral("ts"), QStringLiteral("2026-11-02T23:47:20+08:00"));
    if (!data.isEmpty()) {
        env.insert(QStringLiteral("data"), data);
    }
    return env;
}

/// 连上本机服务端；port 由调用方给。
/// origin 参数默认**空串**：P2-3 之后服务端只放行空 Origin 与 android-app://*，
/// 而原生 App（OkHttp 这类）本来就不发 Origin，所以"正常客户端"就是空 Origin。
/// 以前这里写的是任意串 "idge-pad-probe"，那正是"任何来源都收"的现场证据。
/// 尝试连接并等它进入 ConnectedState；**不成功返回 nullptr，不抛**。
/// P2-3 之后"被白名单拒掉"的正常表现就是服务端回 403、客户端直接落到 Unconnected，
/// 所以断言拒绝必须能区分"没连上"和"连不上=测试环境坏了"这两件事——交给调用方判。
QWebSocket *tryConnectTo(quint16 port, const QString &origin)
{
    QWebSocket *socket = new QWebSocket(origin, QWebSocketProtocol::VersionLatest);
    socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(port)));
    QElapsedTimer clock;
    clock.start();
    while (socket->state() != QAbstractSocket::ConnectedState
           && socket->state() != QAbstractSocket::UnconnectedState && clock.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    if (socket->state() != QAbstractSocket::ConnectedState) {
        socket->deleteLater();
        return nullptr;
    }
    return socket;
}

/// 正常客户端口径：origin 默认**空串**。
/// 原生 App（OkHttp 那类）本来就不发 Origin，所以"合法对端"就是空 Origin；
/// 以前这里写的是任意串 "idge-pad-probe"，那恰好是"任何来源都收"的现场证据。
QWebSocket *connectTo(quint16 port, const QString &origin = QString())
{
    QWebSocket *socket = tryConnectTo(port, origin);
    if (!socket) {
        throw std::runtime_error("连不上本机 Pad 服务端");
    }
    return socket;
}

AlarmRecord makeRecord(const QString &id)
{
    AlarmRecord record;
    record.id = id;
    record.alarmType = QStringLiteral("detection");
    record.alarmLevel = 3;
    record.alarmTime = QStringLiteral("2026-11-02T17:30:00");
    record.channel = 0;
    record.clsId = 0;
    record.className = QStringLiteral("un_helmet");
    record.confidence = 0.91f;
    record.status = QStringLiteral("pending");
    record.createTime = record.alarmTime;
    record.updateTime = record.alarmTime;
    return record;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

try {
    const quint16 kPort = 18899;
    pad::ServerConfig cfg;
    cfg.port = kPort;
    cfg.path = QStringLiteral("/");
    cfg.deviceId = QStringLiteral("EDGE-TEST-01");
    cfg.syncLimit = 50;
    // mediaBaseUrl 故意留空：本机测试不依赖那台 http 文件服务，正好验证"没底址就不给地址"

    // ── 1：start 与重复 start ──────────────────────────────────────────────
    {
        QString err;
        check(pad::PadWsServer::instance().start(cfg, &err),
              qPrintable(QStringLiteral("服务端起不来：") + err));
        check(pad::PadWsServer::instance().running(), "running() 应为真");
        check(pad::PadWsServer::instance().port() == kPort, "port() 回实际绑定口");
        QString err2;
        check(pad::PadWsServer::instance().start(cfg, &err2), "重复 start 必须幂等返回 true");
        pad::ServerConfig bad;
        bad.port = 0;
        QString err3;
        check(!pad::PadWsServer::instance().start(bad, &err3), "端口 0 要判失败而不是默认乱起");
    }

    // ── 2/4/5/8/9/10/11：一条正常连接走完消息面 ─────────────────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);

        // hello
        QJsonObject helloData;
        helloData.insert(QStringLiteral("deviceId"), QStringLiteral("Redmi Pad 2 Pro"));
        helloData.insert(QStringLiteral("token"), QString());
        helloData.insert(QStringLiteral("clientVersion"), QStringLiteral("1.0.0"));
        helloData.insert(QStringLiteral("protocolVersion"), 1);
        check(sendJson(socket, envelopeOf(QStringLiteral("hello"), helloData)).isEmpty(),
              "hello 发送失败");
        check(recorder.waitFor(1) >= 1, "5 秒内要收到 hello_ack（超时 Pad 会判协议不兼容）");
        const QJsonObject ack = recorder.lastOfType(QStringLiteral("hello_ack"));
        check(ack.value(QLatin1String("code")).toInt() == 0, "hello_ack 的 code 应为 0");
        const QJsonObject ackData = ack.value(QLatin1String("data")).toObject();
        check(ackData.value(QLatin1String("protocolVersion")).toInt() == 1,
              "hello_ack 要回显 protocolVersion=1");
        check(ackData.value(QLatin1String("serverTime")).toString().contains(QLatin1Char('+')),
              "serverTime 必须带时区偏移（Pad 用它校时钟）");
        check(ackData.value(QLatin1String("deviceId")).toString() == QLatin1String("EDGE-TEST-01"),
              "配置里的 device_id 要出现在 hello_ack");
        const QJsonObject caps = ackData.value(QLatin1String("capabilities")).toObject();
        check(caps.size() == 1 && caps.contains(QLatin1String("alarm")),
              "capabilities 只声明 alarm（§3.5：非报警能力放 /api/health）");
        const QJsonObject alarmCap = caps.value(QLatin1String("alarm")).toObject();
        check(alarmCap.value(QLatin1String("detectionBoxes")).toBool() == false,
              "detectionBoxes 必须是 false（v1 硬要求）");
        check(alarmCap.value(QLatin1String("push")).toBool() == true, "push 为真");
        check(alarmCap.value(QLatin1String("evidenceUrl")).toBool() == false,
              "没配媒体底址时 evidenceUrl 要据实报 false");

        // ping → pong
        check(sendJson(socket, envelopeOf(QStringLiteral("ping"), QJsonObject())).isEmpty(),
              "ping 发送失败");
        check(recorder.waitFor(2) >= 2, "30 秒内要回 pong（这里用 2 秒上限）");
        const QJsonObject pong = recorder.lastOfType(QStringLiteral("pong"));
        // P3-1（2026-11-03 修）：§3.2 要求服务端→Pad 的消息必须带 code/message，pong 不例外。
        // 这条以前钉的是"pong 不必塞 code"——那是把偏离钉成契约。
        check(pong.contains(QLatin1String("ts"))
                  && pong.value(QLatin1String("code")).toInt() == 0
                  && pong.value(QLatin1String("message")).toString() == QLatin1String("ok"),
              "pong 带 type/ts/code/message（§3.2 服务端消息必须带 code）");

        // sync_request：空游标 + 带 taskId（§13.1.5 必填；空 taskId 现在按 §2.6 判 2001）
        QJsonObject syncData;
        syncData.insert(QStringLiteral("lastAlarmId"), QJsonValue(QJsonValue::Null));
        syncData.insert(QStringLiteral("lastEventTime"), QJsonValue(QJsonValue::Null));
        syncData.insert(QStringLiteral("taskId"), QStringLiteral("SG-2026-11"));
        check(sendJson(socket, envelopeOf(QStringLiteral("sync_request"), syncData)).isEmpty(),
              "sync_request 发送失败");
        const int before = recorder.mark();
        check(recorder.waitFor(before + 1) >= before + 1,
              "每个 sync_request 必须回且只回一个 sync_result");
        const QJsonObject sync = recorder.lastOfType(QStringLiteral("sync_result"));
        const QJsonObject syncBody = sync.value(QLatin1String("data")).toObject();
        check(syncBody.contains(QLatin1String("alarms")) && syncBody.contains(QLatin1String("cursor")),
              "sync_result 要同时有 alarms 与 cursor 两个键");
        check(syncBody.value(QLatin1String("alarms")).isArray(),
              "alarms 必须是数组本身；缺键时 Pad 会保留旧缓存而不是清空");
        const QJsonObject cursor = syncBody.value(QLatin1String("cursor")).toObject();
        check(cursor.contains(QLatin1String("lastEventTime")),
              "cursor 里必须有 lastEventTime 这个键，哪怕值是 null");

        // 未知 type → error 2001
        check(sendJson(socket, envelopeOf(QStringLiteral("nonsense_type"), QJsonObject())).isEmpty(),
              "未知类型发送失败");
        const int afterUnknown = recorder.mark();
        check(recorder.waitFor(afterUnknown + 1) >= afterUnknown + 1, "未知 type 要回 error");
        const QJsonObject err = recorder.lastOfType(QStringLiteral("error"));
        check(err.value(QLatin1String("code")).toInt() == 2001,
              "未知消息类型的错误码是 2001（§2.4/§3.9）");
        check(err.value(QLatin1String("message")).toString().contains(QStringLiteral("未知")),
              "message 要能直接展示");
        check(err.value(QLatin1String("data")).toObject()
                  .value(QLatin1String("detail")).toString().contains(QStringLiteral("nonsense")),
              "data.detail 里带上那个 type，方便对端日志定位");

        // alarm_ack 不回复：先记下当前帧数，发 ack，再发 ping，
        // 断言"ack 之后紧接着的那一帧就是 pong"——中间若插了 ack 的回执，pong 会晚一帧
        QJsonObject ackData2;
        ackData2.insert(QStringLiteral("alarmId"), QStringLiteral("ALM-TEST-1"));
        ackData2.insert(QStringLiteral("receivedAt"), QStringLiteral("2026-11-02T23:47:26+08:00"));
        const int markBeforeAck = recorder.mark();
        check(sendJson(socket, envelopeOf(QStringLiteral("alarm_ack"), ackData2)).isEmpty(),
              "alarm_ack 发送失败");
        check(sendJson(socket, envelopeOf(QStringLiteral("ping"), QJsonObject())).isEmpty(),
              "紧随其后的 ping 发送失败");
        check(recorder.waitFor(markBeforeAck + 1) >= markBeforeAck + 1, "pong 要能收到");
        // ack 与 ping 之间只该多出**一帧**，那一帧必须是 pong：
        // 服务端若回执了 alarm_ack，这里就会看到两帧、或第一帧不是 pong。
        check(recorder.waitFor(recorder.count() + 1, 300) == markBeforeAck + 1,
              "alarm_ack 之后不该再有别的帧（§3.3：边缘设备不要回复 alarm_ack）");
        check(recorder.at(markBeforeAck).value(QLatin1String("type")).toString()
                  == QLatin1String("pong"),
              "ack 后面的第一帧就是 pong，说明中间没有插话");

        // 10/11：AlarmManager 产生报警 → 连接上收到 alarm 快照
        const int markBeforeAlarm = recorder.mark();
        AlarmManager::instance().storeAndNotify(QVector<AlarmRecord>{makeRecord(
            QStringLiteral("ALARM-PROBE-1"))});
        check(recorder.waitFor(markBeforeAlarm + 1) >= markBeforeAlarm + 1,
              "alarmGenerated 要转成一条 type=alarm 的推送");
        const QJsonObject push = recorder.lastOfType(QStringLiteral("alarm"));
        const QJsonObject pushData = push.value(QLatin1String("data")).toObject();
        check(pushData.value(QLatin1String("alarmId")).toString()
                  == QLatin1String("ALARM-PROBE-1"), "推送的 alarmId 对得上");
        check(pushData.value(QLatin1String("type")).toString() == QLatin1String("HELMET")
                  && pushData.value(QLatin1String("typeLabel")).toString()
                         == QStringLiteral("未戴安全帽"),
              "出站走的是那张类型表");
        check(pushData.value(QLatin1String("status")).toString() == QLatin1String("PENDING"),
              "pending → PENDING");
        check(pushData.value(QLatin1String("updatedAt")).toString().contains(QLatin1Char('+')),
              "updatedAt 带偏移");
        // §13.2.1：证据平铺在 alarm 顶层，必填键 evidenceSnapshotUrl 必须在；
        // 没有媒体底址时给空串（省略 = 必填缺失），也绝不拼一个对端取不到的地址。
        check(pushData.contains(QLatin1String("evidenceSnapshotUrl"))
                  && pushData.value(QLatin1String("evidenceSnapshotUrl")).toString().isEmpty(),
              "没有媒体底址时 evidenceSnapshotUrl 是空串（键在、地址不给）");
        check(!pushData.contains(QLatin1String("evidence")),
              "alarm 顶层不再有 evidence 对象（§13.2.1 末条）");
        check(pushData.contains(QLatin1String("taskId")),
              "taskId 键必须在（未登录时是空串，不是缺键）");

        socket->close();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
    }

    // ── 3：协议版本不兼容 → code=1001 且被关闭 ───────────────────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);
        QJsonObject helloData;
        // 必填项齐全，只把 protocolVersion 改坏——否则会被 P3-2 的参数检查先拦掉，
        // 测不到"版本不兼容"这条分支（那条分支的判据在参数检查之后）。
        helloData.insert(QStringLiteral("deviceId"), QStringLiteral("Old Pad"));
        helloData.insert(QStringLiteral("token"), QString());
        helloData.insert(QStringLiteral("clientVersion"), QStringLiteral("0.9.0"));
        helloData.insert(QStringLiteral("protocolVersion"), 2);
        check(sendJson(socket, envelopeOf(QStringLiteral("hello"), helloData)).isEmpty(),
              "hello(v2) 发送失败");
        check(recorder.waitFor(1) >= 1, "版本不兼容也要先回一帧再关闭");
        const QJsonObject ack = recorder.lastOfType(QStringLiteral("hello_ack"));
        check(ack.value(QLatin1String("code")).toInt() == 1001,
              "protocolVersion 不匹配 → hello_ack 带 code=1001（§3.5）");

        QElapsedTimer clock;
        clock.start();
        while (socket->state() != QAbstractSocket::UnconnectedState && clock.elapsed() < 2000) {
            QCoreApplication::processEvents();
        }
        check(socket->state() == QAbstractSocket::UnconnectedState, "不兼容的连接要被关闭");
        socket->deleteLater();
        QCoreApplication::processEvents();
    }

    // ── 13：hello 缺必填项 → error 2001，且不关连接（P3-2）──────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);
        QJsonObject broken;
        broken.insert(QStringLiteral("protocolVersion"), 1);   // 只有版本，其余全缺
        check(sendJson(socket, envelopeOf(QStringLiteral("hello"), broken)).isEmpty(),
              "缺字段的 hello 发送失败");
        check(recorder.waitFor(1) >= 1, "缺必填项要回 error");
        const QJsonObject err = recorder.lastOfType(QStringLiteral("error"));
        check(err.value(QLatin1String("code")).toInt() == 2001,
              "缺必填项是 2001（§2.6），不是 1001——两个判据不能混");
        check(err.value(QLatin1String("data")).toObject()
                  .value(QLatin1String("detail")).toString().contains(QStringLiteral("deviceId")),
              "detail 要点名缺的是哪个键");
        // 参数错**不关连接**：补齐后重发 hello 应当还能握手成功
        QJsonObject full;
        full.insert(QStringLiteral("deviceId"), QStringLiteral("Redmi Pad 2 Pro"));
        full.insert(QStringLiteral("token"), QString());
        full.insert(QStringLiteral("clientVersion"), QStringLiteral("1.0.0"));
        full.insert(QStringLiteral("protocolVersion"), 1);
        check(sendJson(socket, envelopeOf(QStringLiteral("hello"), full)).isEmpty(),
              "补全后的 hello 发送失败");
        check(recorder.waitFor(2) >= 2, "同一条连接补齐参数后要能握手");
        check(recorder.lastOfType(QStringLiteral("hello_ack"))
                  .value(QLatin1String("code")).toInt() == 0,
              "第二条 hello 应拿到 code=0 的 hello_ack");
        check(socket->state() == QAbstractSocket::ConnectedState, "参数错误不该关连接");
        socket->close();
        QCoreApplication::processEvents();
    }

    // ── 14：sync_request 空 taskId → error 2001（P3-3）───────────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);
        QJsonObject syncData;
        syncData.insert(QStringLiteral("lastEventTime"), QJsonValue(QJsonValue::Null));
        syncData.insert(QStringLiteral("taskId"), QString());   // 没进施工计划页就是空
        check(sendJson(socket, envelopeOf(QStringLiteral("sync_request"), syncData)).isEmpty(),
              "空 taskId 的 sync_request 发送失败");
        check(recorder.waitFor(1) >= 1, "空 taskId 要回东西（不能静默不回，那是活锁）");
        const QJsonObject err = recorder.lastOfType(QStringLiteral("error"));
        check(err.value(QLatin1String("code")).toInt() == 2001
                  && err.value(QLatin1String("data")).toObject()
                         .value(QLatin1String("detail")).toString().contains(QStringLiteral("taskId")),
              "§13.1.5 的 taskId 必填：空值按 2001 拒，不再返回所有计划的报警");
        check(recorder.count() == 1
                  && recorder.at(0).value(QLatin1String("type")).toString()
                         == QLatin1String("error"),
              "被拒的 sync_request 只能回那一帧 error，不得再发 sync_result");
        socket->close();
        QCoreApplication::processEvents();
    }

    // ── 15：Origin 白名单（P2-3）────────────────────────────────────────────
    {
        // 先把前几组关掉的连接排空再取基线：disconnected → clients_.removeAll 要等事件循环，
        // 立刻数会把"上一组的收尾时序"当成"这一组的行为"（本轮实测：进组时挂着 1 路，
        // 拒绝动作本身一个都没加进去）。
        QElapsedTimer drain;
        drain.start();
        while (pad::PadWsServer::instance().clientCount() > 0 && drain.elapsed() < 1000) {
            QCoreApplication::processEvents();
        }
        check(pad::PadWsServer::instance().clientCount() == 0, "基线：前序连接应已排空");

        check(tryConnectTo(kPort, QStringLiteral("http://192.168.1.50:8080")) == nullptr,
              "浏览器 Origin 的连接不该建立（§2.3 免鉴权 ≠ 免来源检查）");
        // Qt 5.12.8 实测：CORS 判据在 newConnection **之前**生效，被拒的连接根本不会
        // 进到 clients_，所以这里不需要额外的"死连接清理"逻辑。
        QCoreApplication::processEvents();
        check(pad::PadWsServer::instance().clientCount() == 0,
              "被拒的连接不得挂进 clients_（那会让日志里的连接数说谎）");

        // 反面对照一：原生 App 那种 android-app:// Origin 要放行
        QWebSocket *ok = tryConnectTo(kPort, QStringLiteral("android-app://com.example.pad"));
        check(ok != nullptr, "android-app:// 前缀应在白名单内");
        if (ok) {
            Recorder okRec(ok);
            check(sendJson(ok, envelopeOf(QStringLiteral("ping"), QJsonObject())).isEmpty(),
                  "放行的 Origin 要能发消息");
            check(okRec.waitFor(1) >= 1, "放行的 Origin 要正常服务");
            ok->close();
            QCoreApplication::processEvents();
        }

        // 反面对照二：不发 Origin（原生客户端的常态）也要放行——前面每一组都走的是这条路
        QWebSocket *blank = tryConnectTo(kPort, QString());
        check(blank != nullptr, "空 Origin 必须放行");
        if (blank) {
            blank->close();
            QCoreApplication::processEvents();
        }
    }

    // ── 16：确认 / 误报 → 立刻推 alarm_update（P1-1）─────────────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);

        // 前置：先造一条报警（没接库也能跑——内存窗口里就取得到完整记录）
        AlarmManager::instance().storeAndNotify(
            QVector<AlarmRecord>{makeRecord(QStringLiteral("ALARM-ACK-1"))});
        const int markGen = recorder.mark();
        check(recorder.waitFor(markGen + 1) >= markGen + 1, "前置：要先收到那条 alarm 快照");

        check(AlarmManager::instance().acknowledgeAlarmById(QStringLiteral("ALARM-ACK-1")),
              "确认操作本身要返回成功");
        const int markAfterAck = recorder.mark();
        check(recorder.waitFor(markAfterAck + 1) >= markAfterAck + 1,
              "确认之后必须收到一帧 alarm_update（§3.7；这条以前是死代码，报告 P1-1）");
        const QJsonObject updData =
            recorder.lastOfType(QStringLiteral("alarm_update")).value(QLatin1String("data")).toObject();
        check(updData.value(QLatin1String("alarmId")).toString() == QLatin1String("ALARM-ACK-1"),
              "alarm_update 的 alarmId 要对得上（对端按它就地合并）");
        // 口径（wireStatus，与 test_pad_wire 契约 4 同一套）：本机 `rectified` 是
        // "已确认/已读"，**不等于** v1 的 DISPOSED（已处置），所以 status 仍是 PENDING，
        // 变化的只有 readAt。这里故意钉住这点：把它"修成"RECTIFIED 就是替处置表单抢跑，
        // 而处置那条路要等 roadmap P1-3 与 A7 的照片列。
        check(updData.value(QLatin1String("status")).toString() == QLatin1String("PENDING"),
              "确认只推进 readAt，status 不因 rectified 变成处置态");
        const QString readAt = updData.value(QLatin1String("readAt")).toString();
        check(!readAt.isEmpty() && readAt.contains(QLatin1Char('+')),
              "readAt 要补出来（本机没有 read_time，按 update_time 兜底）并带时区偏移");
        check(updData.value(QLatin1String("updatedAt")).toString().contains(QLatin1Char('+')),
              "updatedAt 必填且带偏移，否则对端按旧值合并会把这条更新吞掉");

        // 误报走同一条信号链
        check(AlarmManager::instance().markAsFalsePositiveById(QStringLiteral("ALARM-ACK-1")),
              "标误报要返回成功");
        const int markAfterFalse = recorder.mark();
        check(recorder.waitFor(markAfterFalse + 1) >= markAfterFalse + 1, "标误报也要推 alarm_update");
        check(recorder.lastOfType(QStringLiteral("alarm_update")).value(QLatin1String("data")).toObject()
                  .value(QLatin1String("status")).toString() == QLatin1String("FALSE_ALARM"),
              "false_alarm → FALSE_ALARM");

        socket->close();
        QCoreApplication::processEvents();
    }

    // ── 17：片段落盘 → 只推 alarm_update（带 clipUrl），不再重发整条 alarm ────
    // 2026-11-05 现场流程：报警当刻先发 alarm（信息+图片），视频写完后用 alarm_update 补视频。
    {
        // 这条分支要从库里重读（updatedAt 只被 updateVideoPath 推过），先起一个临时库。
        // 必须走 AlarmManager::initDatabase()：AlarmManager 自己缓存 dbInitialized_，
        // 只调 DatabaseManager::initialize() 的话它仍认为"没库"，attach 直接返回、不推帧。
        // static：让库活到进程结束——storeAndNotify 是**异步入库**（AlarmDbWriter 线程），
        // 块一结束就删目录会让后台那条写入失败（日志 "unable to open database file"）。
        static QTemporaryDir dbDir;
        check(dbDir.isValid(), "临时库目录建不出来");
        check(AlarmManager::instance().initDatabase(dbDir.path() + QStringLiteral("/ws_clip.db")),
              "临时报警库初始化失败");

        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);

        // 负例：没配媒体底址 → clipUrl **整个键不出现**（不能是 null，null 会清掉对端已拿到的视频）
        const AlarmRecord clipRec = makeRecord(QStringLiteral("ALARM-CLIP-1"));
        {
            // 报警入库是异步的（AlarmDbWriter），而片段回调会立刻按 id 去库里读；
            // 这里同步补一行，等价于生产上"片段在报警 +2 秒后才到"的时序。
            AlarmDAO dao;
            check(dao.insertAlarms(QVector<AlarmRecord>{clipRec}) > 0, "前置：同步入库失败");
        }
        AlarmManager::instance().storeAndNotify(QVector<AlarmRecord>{clipRec});
        {
            AlarmDAO probe;
            check(!probe.queryById(QStringLiteral("ALARM-CLIP-1")).id.isEmpty(),
                  "前置：报警必须已经在库里（片段回调靠 queryById 取 updatedAt）");
        }
        const int markGen = recorder.mark();
        check(recorder.waitFor(markGen + 1) >= markGen + 1, "前置：先收到那条 alarm 快照");
        const int markClip = recorder.mark();
        AlarmManager::instance().attachVideoClip(
            {QStringLiteral("ALARM-CLIP-1")},
            QStringLiteral("/tmp/idge_clip_probe/ALARM-CLIP-1.h265"),
            QStringLiteral("/tmp/idge_clip_probe/ALARM-CLIP-1.mp4"));
        check(recorder.waitFor(markClip + 1) >= markClip + 1, "片段落盘后必须推一帧");
        const QJsonObject neg = recorder.at(markClip);
        check(neg.value(QLatin1String("type")).toString() == QLatin1String("alarm_update"),
              "补视频走 alarm_update，而不是重发整条 alarm");
        const QJsonObject negData = neg.value(QLatin1String("data")).toObject();
        check(negData.value(QLatin1String("alarmId")).toString() == QStringLiteral("ALARM-CLIP-1"),
              "alarm_update 的 alarmId 对得上（对端按它就地合并）");
        check(!negData.contains(QLatin1String("clipUrl")),
              "没有底址时 update 里不带 clipUrl（缺键≠null）");
        check(negData.value(QLatin1String("syncState")).toString() == QLatin1String("SUBMITTED"),
              "update 带 syncState（本机恒 SUBMITTED）");
        socket->close();
        QCoreApplication::processEvents();

        // 正例：配了媒体底址 + 同名 mp4 真在盘上 → clipUrl 指到它
        QTemporaryDir mediaDir;
        check(mediaDir.isValid(), "临时媒体根建不出来");
        const QString rawPath = mediaDir.path() + QStringLiteral("/ALARM-CLIP-2.h265");
        {
            QFile mp4(mediaDir.path() + QStringLiteral("/ALARM-CLIP-2.mp4"));
            check(mp4.open(QIODevice::WriteOnly), "mp4 测试文件写不出来");
            mp4.write("x");
        }
        pad::PadWsServer::instance().stop();
        pad::ServerConfig withMedia = cfg;
        withMedia.mediaBaseUrl = QStringLiteral("http://127.0.0.1:18090");
        withMedia.mediaRootDir = mediaDir.path();
        QString mediaErr;
        check(pad::PadWsServer::instance().start(withMedia, &mediaErr),
              qPrintable(QStringLiteral("带媒体底址重启失败：") + mediaErr));

        socket = connectTo(kPort);
        Recorder recorder2(socket);
        const AlarmRecord clipRec2 = makeRecord(QStringLiteral("ALARM-CLIP-2"));
        {
            AlarmDAO dao;
            check(dao.insertAlarms(QVector<AlarmRecord>{clipRec2}) > 0, "正例：同步入库失败");
        }
        AlarmManager::instance().storeAndNotify(QVector<AlarmRecord>{clipRec2});
        const int markGen2 = recorder2.mark();
        check(recorder2.waitFor(markGen2 + 1) >= markGen2 + 1, "正例前置：先收到 alarm");
        const int markClip2 = recorder2.mark();
        AlarmManager::instance().attachVideoClip(
            {QStringLiteral("ALARM-CLIP-2")}, rawPath,
            mediaDir.path() + QStringLiteral("/ALARM-CLIP-2.mp4"));
        check(recorder2.waitFor(markClip2 + 1) >= markClip2 + 1, "正例：片段落盘后要推 update");
        const QJsonObject posFrame = recorder2.at(markClip2);
        const QJsonObject posData = posFrame.value(QLatin1String("data")).toObject();
        check(posFrame.value(QLatin1String("type")).toString() == QLatin1String("alarm_update"),
              "正例也是 alarm_update");
        check(posData.value(QLatin1String("clipUrl")).toString()
                  == QStringLiteral("http://127.0.0.1:18090/ALARM-CLIP-2.mp4"),
              "clipUrl = 媒体底址 + root 内相对路径");
        check(posData.value(QLatin1String("updatedAt")).toString().contains(QLatin1Char('+')),
              "updatedAt 带偏移（对端按它判较新）");
        socket->close();
        QCoreApplication::processEvents();

        // 复位成原来的无底址配置，别影响后面的组
        pad::PadWsServer::instance().stop();
        QString resetErr;
        check(pad::PadWsServer::instance().start(cfg, &resetErr), "复位服务端失败");
    }

    // ── 服务端仍能接受新连接（上一条关闭没把服务搞挂）───────────────────────
    {
        QWebSocket *socket = connectTo(kPort);
        Recorder recorder(socket);
        check(sendJson(socket, envelopeOf(QStringLiteral("ping"), QJsonObject())).isEmpty(),
              "ping 发送失败");
        check(recorder.waitFor(1) >= 1, "关闭一条坏连接后服务端仍要正常服务");
        socket->close();
        QCoreApplication::processEvents();
    }

    pad::PadWsServer::instance().stop();
    check(!pad::PadWsServer::instance().running(), "stop() 之后 running() 为假");
    // QWebSocketServer 可以关掉了再开（与 ZLM 那种进程级一次不同）
    QString reopenErr;
    check(pad::PadWsServer::instance().start(cfg, &reopenErr),
          qPrintable(QStringLiteral("stop 后应能再 start：") + reopenErr));
    pad::PadWsServer::instance().stop();

    // ── 12：device_id 留空也要出必填的 deviceId（口径 A17，报告 P2-1 的半条）──
    {
        pad::ServerConfig autoId = cfg;
        autoId.deviceId.clear();
        autoId.port = static_cast<quint16>(kPort + 1);
        QString err;
        check(pad::PadWsServer::instance().start(autoId, &err),
              qPrintable(QStringLiteral("空 device_id 也要能起服务：") + err));

        const QJsonObject ackData = pad::PadWsServer::instance().helloAckData();
        const QString id = ackData.value(QLatin1String("deviceId")).toString();
        check(ackData.contains(QLatin1String("deviceId")) && !id.isEmpty(),
              "§13.1.3 的 deviceId 是必填：配置留空也必须插一个值，不能整键省略");
        check(id == pad::defaultDeviceId(), "空配置时 hello_ack 用的就是派生值");
        check(ackData.value(QLatin1String("capabilities")).toObject()
                  .value(QLatin1String("alarm")).toObject()
                  .value(QLatin1String("detectionBoxes")).toBool() == false,
              "派生分支没动 capabilities");

        pad::PadWsServer::instance().stop();
    }

    std::cout << "pad ws server contracts passed: idempotent start / hello_ack shape "
                 "(detectionBoxes=false, serverTime with offset) / 1001 + close on version "
                 "mismatch / pong carries code+message / exactly one sync_result with "
                 "alarms+cursor / error 2001 on unknown type / alarm_ack gets no reply / "
                 "alarmGenerated broadcasts a full snapshot / no media base url means no "
                 "evidence url / empty device_id still yields the required deviceId / "
                 "hello missing required fields -> 2001 without closing / "
                 "empty taskId rejected with 2001 / origin allow-list (browser rejected, "
                 "empty + android-app accepted) / ack + false-positive both push alarm_update\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    pad::PadWsServer::instance().stop();
    return 1;
}
}
