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
// 地址口径：**只填 ws://**。本机 Qt 的 OpenSSL 符号解析缺两个（见 plan/roadmap.md §十 T-1），
// 填 wss:// 会走 TLS 握手路径，那两处的函数指针为空。
// 消息不做排队：未连接时 sendJson 直接丢弃并记 warning。
// 出站还有第二个身份：本类实现 UplinkTransport，把本机报警（JSON 控制面 + 媒体二进制帧）
// 交给 AlarmUplink 排队发送，那条路有自己的队列与水位闸门，不走 sendJson。
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

/**
 * @brief 读配置里的 server_url 并异步发起握手
 *
 * 每次连接都现读 ConfigManager（不缓存地址）：设置页改完 url 之后 stop()+start() 就会
 * 走新地址，不需要重启进程。地址为空/URL 非法只发 logMessage，不置任何错误状态——
 * 本类的"失败"只体现为"连不上 + 日志"，调用方没有可查询的错误码。
 * open() 是异步的：返回时还没连上，成功与否只能等 connected/error 信号。
 */
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

// 四个发送方的报文形状都在这里定死：字段名与对端约定一致（alarm_id / last_alarm_id /
// stream_id / fence），未连接时一律走 sendJson 的"丢弃 + warning"，本机不排队。
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

// 注意 stream_id 发的是**字符串**（QString::number）而不是 JSON number——这是服务端当时的
// 解析口径，本类没有做校验，改这里要先确认对端。x1/y1/x2/y2 是矩形对角点，
// 与本机 geofence 的任意多边形模型不兼容（见 ws_client.h），所以全工程零调用方。
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

/**
 * @brief 唯一的出站文本出口：未连接即丢弃
 *
 * 这个"丢弃"语义只属于 ping/ack/sync_request/get_streams/set_fence 这五个发送方，
 * 报警上行那一路要的是排队，走下面 UplinkTransport 那组接口，不共用此函数。
 * 无返回值：调用方无法感知丢包，所以别把需要可靠送达的东西接到这里。
 * 序列化用 Compact（无缩进无换行）：一行一帧，对端按整段 JSON 解析。
 */
void WsClient::sendJson(const QJsonObject &obj)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        emit logMessage("warning", "WebSocket: 未连接，消息未发送");
        return;
    }
    QJsonDocument doc(obj);
    socket_->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

// ============================================================================
// UplinkTransport：报警上行的出站口
// ----------------------------------------------------------------------------
// 与 sendJson 的区别是刻意的：上面那个出口"未连接即丢弃"，是 ping/ack/
// sync_request/get_streams/set_fence 四个既有发送方的行为，动它们会改现有语义；
// 上行的排队在 AlarmUplinkEngine 里做，这里只负责"能不能写、写了多少"。
// 返回值一律用 Qt 的实际写入字节数（失败为 -1），部分写/失败由调用方决定重试，
// 本层不做任何缓冲。
// ============================================================================

bool WsClient::readyForMore() const
{
    return socket_->bytesToWrite() < kWriteWatermarkBytes;
}

qint64 WsClient::sendText(const QString &text)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return -1;
    }
    return socket_->sendTextMessage(text);
}

qint64 WsClient::sendBinary(const QByteArray &bytes)
{
    if (socket_->state() != QAbstractSocket::ConnectedState) {
        return -1;
    }
    // Qt 5.12 的 QWebSocket 私有实现里，一次 sendBinaryMessage = 一次 doWriteFrames，
    // 没有分片循环：所以一帧就是链路上的一个数据帧，块大小必须由上层（chunk_size）控制，
    // 不能指望 Qt 帮我们切。见 plan/roadmap.md 的 P0-1 说明。
    return socket_->sendBinaryMessage(bytes);
}

/**
 * @brief 连上之后：起心跳表 + 补一次 sync_request
 *
 * 心跳间隔每次连接都从配置现读并重新 start()，所以改 heartbeat_interval 后
 * 下一次重连即生效（运行期不会主动重读，定时器一直跑到断开）。
 * lastAlarmId_ 为空（本机从没收到过 alarm）时不补发同步请求：
 * 对空 id 的 sync_request 服务端行为未定，别拿它当"上线打招呼"。
 */
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

/**
 * @brief 断开：停心跳，非主动停时挂一次重连定时器
 *
 * 重连表是**单发**的：一次只约一次尝试，下一轮要等再次收到 disconnected() 才会再挂。
 * NOTE(未核实): 若某次连接尝试只发了 error() 而没发 disconnected()（Qt 在握手失败时
 * 的信号次序没在本机核实过，onError 里也不挂表），重连链会断在这里、只剩一条日志；
 * 现场遇到"服务端重启后本机再也不连"就先查这一条。
 * stopping_ 是唯一区分"主动 stop"与"链路掉"的依据，所以 stop() 里必须先置它再 close()。
 */
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

/**
 * @brief 收到的文本帧按 type 分发（本机这条链路的唯一入站口）
 *
 * 三条与"宽容协议"有关的口径：
 *   - 解析失败/未知 type 都只记日志，**不回错误也不断连**：这条链是旁路观测，
 *     对端实现不严格时不该把连接搞死；
 *   - alarm 分支先记 lastAlarmId_ 再转发，且 alarm_id 为空时**不覆盖**旧值
 *     （否则一条脏数据会把补漏游标清掉，重连后的 sync_request 变成全量）；
 *   - 不校验 data 的结构，原样透传给 frmMain：本机是 Pad 侧的显示方，字段解释归它。
 * uplink_ack 同样只拆五个字段就转发，认领与重试判断在 AlarmUplinkEngine 里。
 */
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
    } else if (type == "uplink_ack") {
        // 报警上行的确认。这里只拆字段，"认领哪一项、算不算成功"全在引擎里判
        const QJsonObject data = obj["data"].toObject();
        emit uplinkAcked(data["message_id"].toString(),
                         data["media_id"].toString(),
                         data["phase"].toString(),
                         data["status"].toString(),
                         data["error"].toString());
    } else {
        emit logMessage("info", QString("WebSocket: 未知消息类型: %1").arg(type));
    }
}

// 主动 stop 过程中的失败是预期的（close 之后还会来一个 error），所以按 stopping_ 静音，
// 免得退出流程刷一屏红字被当成故障。error 参数本身不区分类型，一律靠 errorString() 给人看。
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
    // 当前口径：server_url 只填 ws://，明文路径不会触发本回调，这里是死代码。
    // 若将来启用 wss://：① 本机 Qt 的 TLS 后端还缺 SSL_get_peer_certificate/EVP_PKEY_base_id
    // 两个符号，握手路径本身就不安全（见 plan/roadmap.md §十 T-1）；② 这句无条件放行
    // （自签/过期/域名不符都连）必须同时改成按指纹白名单或直接拒绝。
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
