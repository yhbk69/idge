// ============================================================================
// pad_ws_server.cpp — 见 pad_ws_server.h
// ============================================================================

#include "pad_ws_server.h"

#include "alarm_dao.h"
#include "pad_wire.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

namespace pad {

namespace {

// v1 §3.5：协议版本是唯一门禁。Pad 现在发 1，将来加版本只改这一处判据。
const int kProtocolVersion = 1;

QString peerText(QWebSocket *socket)
{
    const QHostAddress addr = socket->peerAddress();
    return QStringLiteral("%1:%2").arg(addr.toString()).arg(socket->peerPort());
}

} // namespace

PadWsServer &PadWsServer::instance()
{
    static PadWsServer s;
    return s;
}

PadWsServer::PadWsServer(QObject *parent) : QObject(parent) {}

PadWsServer::~PadWsServer()
{
    stop();
}

bool PadWsServer::start(const ServerConfig &cfg, QString *err)
{
    // 先验参数再判幂等：否则"已经在跑了"会把一个根本不能用的配置（port=0）也回成 true，
    // 装配处的日志就会写着"已监听端口 0"，现场对着一台没起来的服务排不到底。
    if (cfg.port == 0) {
        if (err) {
            *err = QStringLiteral("pad_server 端口无效（0）");
        }
        return false;
    }
    if (server_ && server_->isListening()) {
        return true;    // 幂等：装配处重复调用不该把已连的 Pad 踢掉
    }

    cfg_ = cfg;
    server_ = new QWebSocketServer(QStringLiteral("idge-pad-v1"),
                                   QWebSocketServer::NonSecureMode, this);
    // path 参数只在日志里出现：QWebSocketServer 不按路径筛连接，Pad 连哪个路径都收。
    if (!server_->listen(QHostAddress::Any, cfg_.port)) {
        if (err) {
            *err = QStringLiteral("端口 %1 起不来：%2").arg(cfg_.port)
                       .arg(server_->errorString());
        }
        server_->deleteLater();
        server_ = nullptr;
        return false;
    }

    connect(server_, &QWebSocketServer::newConnection, this, &PadWsServer::onNewConnection);

    if (!alarmsWired_) {
        // AutoConnection：alarmGenerated 来自解码线程、videoClipAttached 来自片段线程，
        // 跨线程时 Qt 自动排队到本对象所在的 GUI 线程，所以收发都只在一根线程上。
        connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
                this, &PadWsServer::handleAlarmGenerated);
        connect(&AlarmManager::instance(), &AlarmManager::videoClipAttached,
                this, &PadWsServer::handleVideoClipAttached);
        alarmsWired_ = true;
    }

    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("Pad 服务端已监听 0.0.0.0:%1%2（明文 ws，零鉴权）")
                        .arg(cfg_.port).arg(cfg_.path));
    return true;
}

void PadWsServer::stop()
{
    for (QWebSocket *socket : clients_) {
        socket->close();
    }
    clients_.clear();

    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
}

bool PadWsServer::running() const
{
    return server_ && server_->isListening();
}

int PadWsServer::clientCount() const
{
    return clients_.size();
}

quint16 PadWsServer::port() const
{
    return running() ? cfg_.port : 0;
}

QJsonObject PadWsServer::helloAckData() const
{
    QJsonObject alarm;
    alarm.insert(QStringLiteral("push"), true);
    alarm.insert(QStringLiteral("detectionBoxes"), false);   // v1 硬要求 false
    alarm.insert(QStringLiteral("evidenceUrl"), !cfg_.mediaBaseUrl.isEmpty());

    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("alarm"), alarm);     // §3.5：只声明报警能力

    QJsonObject data;
    data.insert(QStringLiteral("serverTime"), wireNow());
    data.insert(QStringLiteral("protocolVersion"), kProtocolVersion);
    if (!cfg_.deviceId.isEmpty()) {
        data.insert(QStringLiteral("deviceId"), cfg_.deviceId);
    }
    data.insert(QStringLiteral("capabilities"), capabilities);
    return data;
}

void PadWsServer::onNewConnection()
{
    while (server_ && server_->hasPendingConnections()) {
        QWebSocket *socket = server_->nextPendingConnection();
        if (!socket) {
            continue;
        }
        // 所有权归本类：Qt 的 echo server 例子就是 disconnected → deleteLater，
        // 不删就是每根连接漏一只 QWebSocket（tests/ws_sink 那种写法只删了自己的包装）。
        connect(socket, &QWebSocket::textMessageReceived, this, &PadWsServer::onTextMessage);
        connect(socket, &QWebSocket::disconnected, this, &PadWsServer::onClientDisconnected);
        // Qt 5.12 上 QWebSocket 的错误信号仍是重载的 error()（socketError 是 5.14+ 才有的名字，
        // 与 ws_client.cpp:39 同一套写法）
        connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), socket,
                [this, socket](QAbstractSocket::SocketError) {
                    emit logMessage(QStringLiteral("pad"),
                                    QStringLiteral("连接 %1 出错：%2")
                                        .arg(peerText(socket), socket->errorString()));
                });
        clients_.append(socket);
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("Pad 已连接 %1（当前 %2 路）")
                            .arg(peerText(socket)).arg(clients_.size()));
    }
}

void PadWsServer::onClientDisconnected()
{
    QWebSocket *socket = qobject_cast<QWebSocket *>(sender());
    if (!socket) {
        return;
    }
    clients_.removeAll(socket);
    socket->deleteLater();
    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("Pad 断开，剩余 %1 路").arg(clients_.size()));
}

void PadWsServer::onTextMessage(const QString &text)
{
    QWebSocket *socket = qobject_cast<QWebSocket *>(sender());
    if (!socket) {
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 不是 JSON 就回 2001：v1 说"未知 type 返回 error 2001 或忽略；不能崩溃"
        sendText(socket, errorEnvelope(2001, QStringLiteral("消息格式错误"),
                                      QStringLiteral("len=%1").arg(text.size())));
        return;
    }

    const QJsonObject env = doc.object();
    const QString type = env.value(QStringLiteral("type")).toString();
    const QJsonObject data = env.value(QStringLiteral("data")).toObject();

    if (type == QLatin1String("hello")) {
        handleHello(socket, data);
        return;
    }
    if (type == QLatin1String("ping")) {
        sendText(socket, pongEnvelope());
        return;
    }
    if (type == QLatin1String("pong")) {
        return;     // 本机不主动 ping；收到就当对端活着
    }
    if (type == QLatin1String("sync_request")) {
        handleSyncRequest(socket, data);
        return;
    }
    if (type == QLatin1String("alarm_ack")) {
        // 只表示"Pad 收到了"，不回任何东西（v1 §3.3 明确"边缘设备不要回复 alarm_ack"）
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("送达确认 alarm=%1")
                            .arg(data.value(QStringLiteral("alarmId")).toString()));
        return;
    }

    emit logMessage(QStringLiteral("pad"), QStringLiteral("未知消息 type=%1，忽略").arg(type));
    sendText(socket, errorEnvelope(2001, QStringLiteral("未知消息类型"),
                                   QStringLiteral("type=%1").arg(type)));
}

void PadWsServer::handleHello(QWebSocket *socket, const QJsonObject &data)
{
    const int peerVersion = data.value(QStringLiteral("protocolVersion")).toInt(0);
    if (peerVersion != kProtocolVersion) {
        // §3.2：不兼容时 hello_ack 带 code=1001 或直接 close(1000)。这里两个都做，
        // 让 Pad 那侧无论按哪条判据都能停业务并提示升级。
        sendText(socket, envelopeWithCode(QStringLiteral("hello_ack"), 1001,
                                          QStringLiteral("协议版本不兼容"), helloAckData()));
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("拒绝连接：对端 protocolVersion=%1，本机=%2")
                            .arg(peerVersion).arg(kProtocolVersion));
        socket->close();
        return;
    }
    sendText(socket, envelope(QStringLiteral("hello_ack"), helloAckData()));
    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("hello 来自 %1（deviceId=%2 client=%3）")
                        .arg(peerText(socket),
                             data.value(QStringLiteral("deviceId")).toString(),
                             data.value(QStringLiteral("clientVersion")).toString()));
}

void PadWsServer::handleSyncRequest(QWebSocket *socket, const QJsonObject &data)
{
    const QString taskId = data.value(QStringLiteral("taskId")).toString();
    const QString cursorWire = data.value(QStringLiteral("lastEventTime")).toString();

    // §3.8：唯一过滤依据是 lastEventTime；lastAlarmId 只是辅助字段，本机不参与判断。
    // 空游标 = 全量。游标认不出来时同样按全量处理（internalTimeFromWire 返回空串）。
    const QString sinceInternal = internalTimeFromWire(cursorWire);

    QJsonArray alarms;
    QString lastAlarmId;
    QString maxInternal;
    if (AlarmManager::instance().isDatabaseInitialized()) {
        AlarmDAO dao;
        const QVector<AlarmRecord> rows = dao.queryUpdatedSince(
            sinceInternal, cfg_.syncLimit <= 0 ? 500 : cfg_.syncLimit);
        for (const AlarmRecord &record : rows) {
            // taskId 为空 = Pad 还没进施工计划页（未登录联调阶段），不按计划过滤。
            if (!taskId.isEmpty() && taskIdOf(record) != taskId) {
                continue;
            }
            const QString effective = record.updateTime.isEmpty() ? record.alarmTime
                                                                  : record.updateTime;
            if (effective > maxInternal) {
                maxInternal = effective;
                lastAlarmId = record.id;
            }
            alarms.append(alarmJson(record, cfg_.mediaBaseUrl, cfg_.mediaRootDir));
        }
    }

    QJsonObject cursor;
    if (!maxInternal.isEmpty()) {
        cursor.insert(QStringLiteral("lastEventTime"), wireTime(maxInternal));
        if (!lastAlarmId.isEmpty()) {
            cursor.insert(QStringLiteral("lastAlarmId"), lastAlarmId);
        }
    } else {
        // 没有报警时回传请求里的值（可为 null），不能让对端的游标倒退
        cursor.insert(QStringLiteral("lastEventTime"),
                      cursorWire.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cursorWire));
    }

    QJsonObject result;
    result.insert(QStringLiteral("alarms"), alarms);     // 空库也要给 []，不能省略
    result.insert(QStringLiteral("cursor"), cursor);

    sendText(socket, envelope(QStringLiteral("sync_result"), result));
    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("sync_result：%1 条（taskId=%2 游标=%3）")
                        .arg(alarms.size())
                        .arg(taskId.isEmpty() ? QStringLiteral("-") : taskId)
                        .arg(cursor.value(QStringLiteral("lastEventTime"))
                                 .toString(cursorWire.isEmpty() ? QStringLiteral("null")
                                                                : cursorWire)));
}

void PadWsServer::handleAlarmGenerated(const AlarmRecord &alarm)
{
    pushAlarm(alarm);
}

void PadWsServer::handleVideoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                                          const QString &webPath)
{
    if (alarmIds.isEmpty() || !running()) {
        return;
    }
    Q_UNUSED(webPath);
    Q_UNUSED(videoPath);

    // 片段这一刻才落盘。内存里那条记录的 videoPath 已由 AlarmManager 填好，但 update_time
    // 只在库里被 updateVideoPath 推过 → 从库里重新读一遍，保证对端按 updatedAt 合并能认出来。
    if (!AlarmManager::instance().isDatabaseInitialized()) {
        return;
    }
    AlarmDAO dao;
    for (const QString &id : alarmIds) {
        const AlarmRecord record = dao.queryById(id);
        if (record.id.isEmpty()) {
            continue;
        }
        pushAlarm(record);
    }
}

void PadWsServer::pushAlarm(const AlarmRecord &record)
{
    if (!running()) {
        return;
    }
    broadcast(envelope(QStringLiteral("alarm"),
                       alarmJson(record, cfg_.mediaBaseUrl, cfg_.mediaRootDir)));
}

void PadWsServer::pushAlarmUpdate(const AlarmRecord &record)
{
    if (!running()) {
        return;
    }
    broadcast(envelope(QStringLiteral("alarm_update"), alarmUpdateJson(record)));
}

void PadWsServer::sendText(QWebSocket *socket, const QByteArray &frame)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    socket->sendTextMessage(QString::fromUtf8(frame));
}

void PadWsServer::broadcast(const QByteArray &frame)
{
    if (clients_.isEmpty()) {
        return;     // 没连着就不补发：对端重连后用 sync_request 一次拉平（v1 §3.8）
    }
    const QString message = QString::fromUtf8(frame);
    for (QWebSocket *socket : clients_) {
        if (socket && socket->state() == QAbstractSocket::ConnectedState) {
            socket->sendTextMessage(message);
        }
    }
}

} // namespace pad
