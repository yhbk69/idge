// ============================================================================
// ws_client.cpp — Pad↔Aibox WebSocket 客户端实现
// ----------------------------------------------------------------------------
// 协议：应用层 JSON 文本帧，收发都靠 "type" 字段分发
//   发：ping / ack / sync_request / get_streams / set_fence
//   收：alarm（记 lastAlarmId_ 后原样转发 data）、pong（忽略）、streams_list，
//       其余只记一条"未知消息类型"日志，不报错、不断连。
// 心跳与重连都是 QTimer 驱动的**应用层**行为：连上才起心跳，断开才起单次重连
// 定时器，间隔取自 websocket.heartbeat_interval / reconnect_interval（秒）。
// 未配置 server_url 时 start() 只打 warning 并返回——程序照常跑，这条链路整个缺席。
// 消息不做排队：未连接时 sendJson 直接丢弃并记 warning。
// ============================================================================

#include "ws_client.h"
// 平铺 include（仓库约定，见 src/README.md）：路径由根 CMakeLists 的
// include_directories 解析，不写 src/ 前缀。
#include "ConfigManager.h"

WsClient &WsClient::instance()
{
    static WsClient client;
    return client;
}

WsClient::WsClient(QObject *parent)
    : QObject(parent)
    , socket_(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , heartbeatTimer_(new QTimer(this))
    , reconnectTimer_(new QTimer(this))
    , stopping_(false)
{
    connect(socket_, &QWebSocket::connected, this, &WsClient::onConnected);
    connect(socket_, &QWebSocket::disconnected, this, &WsClient::onDisconnected);
    connect(socket_, &QWebSocket::textMessageReceived, this, &WsClient::onTextMessageReceived);
    connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &WsClient::onError);
    connect(socket_, &QWebSocket::sslErrors, this, &WsClient::onSslErrors);

    heartbeatTimer_->setSingleShot(false);
    connect(heartbeatTimer_, &QTimer::timeout, this, &WsClient::onHeartbeatTimeout);

    reconnectTimer_->setSingleShot(true);
    connect(reconnectTimer_, &QTimer::timeout, this, &WsClient::onReconnectTimeout);
}

WsClient::~WsClient()
{
    stop();
}

void WsClient::start()
{
    stopping_ = false;
    QString url = ConfigManager::instance().wsServerUrl();
    if (url.isEmpty()) {
        emit logMessage("warning", "WebSocket: 服务器地址未配置");
        return;
    }
    connectToServer();
}

void WsClient::stop()
{
    // 先置 stopping_ 再 close：onDisconnected 靠它区分"主动停"与"链路掉"，
    // 反过来写会让每次主动退出都排一次重连定时器。
    stopping_ = true;
    heartbeatTimer_->stop();
    reconnectTimer_->stop();
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        socket_->close();
    }
}

void WsClient::connectToServer()
{
    QString url = ConfigManager::instance().wsServerUrl();
    if (url.isEmpty()) {
        emit logMessage("warning", "WebSocket: 服务器地址为空");
        return;
    }

    emit logMessage("system", QString("WebSocket: 连接中 %1").arg(url));

    QUrl qurl(url);
    if (!qurl.isValid()) {
        emit logMessage("error", QString("WebSocket: URL 格式错误: %1").arg(url));
        return;
    }

    socket_->open(qurl);
}

void WsClient::sendPing()
{
    QJsonObject obj;
    obj["type"] = "ping";
    sendJson(obj);
}

void WsClient::sendAck(const QString &alarmId)
{
    QJsonObject obj;
    obj["type"] = "ack";
    obj["alarm_id"] = alarmId;
    sendJson(obj);
}

void WsClient::sendSyncRequest(const QString &lastAlarmId)
{
    QJsonObject obj;
    obj["type"] = "sync_request";
    obj["last_alarm_id"] = lastAlarmId;
    sendJson(obj);
}

void WsClient::sendGetStreams()
{
    QJsonObject obj;
    obj["type"] = "get_streams";
    sendJson(obj);
}

void WsClient::sendSetFence(int streamId, double x1, double y1, double x2, double y2)
{
    QJsonObject obj;
    obj["type"] = "set_fence";
    obj["stream_id"] = QString::number(streamId);
    QJsonObject fence;
    fence["x1"] = x1;
    fence["y1"] = y1;
    fence["x2"] = x2;
    fence["y2"] = y2;
    obj["fence"] = fence;
    sendJson(obj);
}

bool WsClient::isConnected() const
{
    return socket_->state() == QAbstractSocket::ConnectedState;
}

void WsClient::sendJson(const QJsonObject &obj)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        emit logMessage("warning", "WebSocket: 未连接，消息未发送");
        return;
    }
    QJsonDocument doc(obj);
    socket_->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void WsClient::onConnected()
{
    emit logMessage("system", "WebSocket: 已连接");
    emit connected();

    int interval = ConfigManager::instance().wsHeartbeatInterval();
    heartbeatTimer_->start(interval * 1000);

    // 重连后同步遗漏报警
    if (!lastAlarmId_.isEmpty()) {
        sendSyncRequest(lastAlarmId_);
    }
}

void WsClient::onDisconnected()
{
    heartbeatTimer_->stop();
    emit logMessage("warning", "WebSocket: 连接断开");
    emit disconnected();

    if (!stopping_) {
        int interval = ConfigManager::instance().wsReconnectInterval();
        emit logMessage("system", QString("WebSocket: %1 秒后重连").arg(interval));
        reconnectTimer_->start(interval * 1000);
    }
}

void WsClient::onTextMessageReceived(const QString &message)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        emit logMessage("warning", QString("WebSocket: JSON 解析失败: %1").arg(err.errorString()));
        return;
    }

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "alarm") {
        QJsonObject data = obj["data"].toObject();
        QString alarmId = data["alarm_id"].toString();
        if (!alarmId.isEmpty()) {
            lastAlarmId_ = alarmId;
        }
        emit alarmReceived(data);
    } else if (type == "pong") {
        // 心跳响应，无需处理
    } else if (type == "streams_list") {
        QJsonArray streams = obj["data"].toArray();
        emit streamsReceived(streams);
    } else {
        emit logMessage("info", QString("WebSocket: 未知消息类型: %1").arg(type));
    }
}

void WsClient::onError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);
    if (!stopping_) {
        emit logMessage("error", QString("WebSocket: 错误 - %1").arg(socket_->errorString()));
    }
}

void WsClient::onSslErrors(const QList<QSslError> &errors)
{
    Q_UNUSED(errors);
    // ⚠ 无条件放行所有证书错误（自签/过期/域名不符都连），wss:// 下等于放弃证书校验，
    // 只适合内网调试。对接公网 Aibox 前应改为按指纹白名单或干脆拒绝。
    // 注：地址填 ws:// 时走明文，本回调不会被触发。
    socket_->ignoreSslErrors();
}

void WsClient::onHeartbeatTimeout()
{
    sendPing();
}

void WsClient::onReconnectTimeout()
{
    if (!stopping_) {
        connectToServer();
    }
}
