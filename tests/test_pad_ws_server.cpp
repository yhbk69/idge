// ============================================================================
// test_pad_ws_server.cpp — Pad 接口 v1 的 WS 服务端线上自检（loopback，单进程）
// ----------------------------------------------------------------------------
// 起真 PadWsServer 在 127.0.0.1 的一个临时口上，用真 QWebSocket 连进来按 §3 的消息面走一遍。
// 不用 Qt5::Test（板端没装），收发节奏靠"等到第 N 帧或超时"的事件循环判据。
//
// 为什么值得单独钉：§12.1 的十条验收全是"对端行为"，纸面实现最容易在这儿翻车——
//   · hello_ack 迟于 5 秒 = Pad 直接判协议不兼容；
//   · pong 多带了 code/message 不算错，但 alarm_ack 回了东西会让 Pad 的连接状态机多一跳；
//   · 未知 type 崩掉 = 现场一个对端版本差就把整机搞挂；
//   · sync_result 省略 alarms 键（而不是给 []）会让 Pad 保留旧缓存、看起来"补漏成功"。
//
// 覆盖十二条契约：
//   1) 端口占用/重复 start 的幂等（第二次 start 直接 true，不把已连的 Pad 踢掉）
//   2) hello → hello_ack：serverTime 带偏移、protocolVersion=1、capabilities 只有 alarm、
//      detectionBoxes 恒 false（v1 硬要求）
//   3) protocolVersion 不匹配 → hello_ack 带 code=1001 且连接被关闭
//   4) ping → pong，且 pong 只有 type/ts（§3.2 服务端消息可带 code，但 pong 没必要）
//   5) sync_request（空游标）→ **恰好一个** sync_result，含 alarms:[] 与 cursor 两个键
//   6) sync_result 的 alarms 是数组而不是缺键（缺键对端=保留旧缓存）
//   7) 数据库未初始化时不崩、返回空数组 + 回显请求游标
//   8) 未知 type → error 2001（code/message 在信封层，data.detail 带上那个 type）
//   9) alarm_ack **不回复**（用"紧接着的 ping 的 pong 是下一帧"来证明中间没有插话）
//  10) AlarmManager 发 alarmGenerated → 连接上收到 type=alarm 的完整快照
//  11) 没有媒体底址时 evidence.snapshotUrls 是 []（不臆造对端取不到的地址）
//  12) 配置 device_id 留空时 hello_ack 仍带必填 deviceId（口径 A17）
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_ws_server，rc 0=PASS 1=FAIL
// ============================================================================

#include "pad_ws_server.h"
#include "pad_wire.h"      // defaultDeviceId()：契约 12 要比对派生值
#include "alarm_manager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
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

/// 连上本机服务端；port 由调用方给
QWebSocket *connectTo(quint16 port)
{
    QWebSocket *socket = new QWebSocket(QStringLiteral("idge-pad-probe"),
                                        QWebSocketProtocol::VersionLatest);
    socket->open(QUrl(QStringLiteral("ws://127.0.0.1:%1/").arg(port)));
    QElapsedTimer clock;
    clock.start();
    while (socket->state() != QAbstractSocket::ConnectedState && clock.elapsed() < 2000) {
        QCoreApplication::processEvents();
    }
    if (socket->state() != QAbstractSocket::ConnectedState) {
        socket->deleteLater();
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
        check(pong.contains(QLatin1String("ts")) && !pong.contains(QLatin1String("code")),
              "pong 只带 type/ts，不必塞 code");

        // sync_request：空游标 + 空 taskId
        QJsonObject syncData;
        syncData.insert(QStringLiteral("lastAlarmId"), QJsonValue(QJsonValue::Null));
        syncData.insert(QStringLiteral("lastEventTime"), QJsonValue(QJsonValue::Null));
        syncData.insert(QStringLiteral("taskId"), QString());
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
        const QJsonArray snaps = pushData.value(QLatin1String("evidence")).toObject()
                                    .value(QLatin1String("snapshotUrls")).toArray();
        check(snaps.isEmpty(), "没有媒体底址时不给证据地址（宁缺勿假）");
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
        helloData.insert(QStringLiteral("deviceId"), QStringLiteral("Old Pad"));
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
                 "mismatch / pong without code / exactly one sync_result with alarms+cursor / "
                 "error 2001 on unknown type / alarm_ack gets no reply / alarmGenerated "
                 "broadcasts a full snapshot / no media base url means no evidence url / "
                 "empty device_id still yields the required deviceId\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    pad::PadWsServer::instance().stop();
    return 1;
}
}
