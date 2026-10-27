#ifndef WS_CLIENT_H
#define WS_CLIENT_H

/**
 * @file ws_client.h
 * @brief Pad↔Aibox 的 WebSocket 客户端（单例，GUI 线程内运行）
 *
 * 面向"本程序作为 Pad 端、远端 Aibox 作为服务与报警源"的对接链路：连接、
 * 收报警推送、断线重连、可选的视频流列表查询与围栏下发。协议是**应用层 JSON 文本帧**
 * （`{"type": ...}` 分发），不是 MQTT/STOMP，也没有鉴权字段。
 *
 * 线程模型：全程单线程。instance() 是函数内 static，第一次调用（frmMain 构造）
 * 发生在 GUI 线程，QWebSocket/QTimer 也都挂在这个线程，无跨线程信号槽。
 * 因此 start()/stop() 只能从 GUI 线程调。
 *
 * 与报警链路的关系：**只读旁路**。收到的远端报警经 frmMain::onWsAlarmReceived 打进
 * 调试页日志并回一条 ack，不写 idge.db、不参与 AlarmManager 限流；本机的围栏多边形
 * （geofence::FenceManager）也不从这里同步——sendSetFence 的矩形参数与本机的任意多边形
 * 模型不兼容，且与 sendGetStreams 一样当前无调用方。协议与现状详见本目录 README.md。
 */
#include <QObject>
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>

/**
 * @brief WebSocket 客户端（单例）
 *
 * Pad 端连接远端 Aibox 服务器，接收报警推送、查询视频流列表、下发围栏配置。
 * 通过 ping/pong 心跳保持连接，断线自动重连。
 */
class WsClient : public QObject
{
    Q_OBJECT

public:
    static WsClient &instance();

    /// 读 websocket.server_url 后异步发起连接；地址为空只打一条 warning 日志即返回
    void start();
    /// 主动断开并抑制自动重连（置 stopping_，frmMain 退出与设置页改地址时调）
    void stop();

    void sendAck(const QString &alarmId);
    /// 重连成功后自动补发（携带 lastAlarmId_），外部暂无调用方
    void sendSyncRequest(const QString &lastAlarmId);
    /// 当前无调用方：streamsReceived 因此在现网配置下不会触发
    void sendGetStreams();
    /// 矩形围栏协议（与本机 geofence 的多边形模型不兼容），当前无调用方
    void sendSetFence(int streamId, double x1, double y1, double x2, double y2);

    bool isConnected() const;
    QString lastAlarmId() const { return lastAlarmId_; }

signals:
    /// 服务端 type=alarm 的 data 字段原样转发，由 frmMain 决定怎么展示
    void alarmReceived(const QJsonObject &data);
    void streamsReceived(const QJsonArray &streams);
    void connected();
    void disconnected();
    /// (category, message)，frmMain 接到自己的调试页日志区
    void logMessage(const QString &category, const QString &message);

private:
    explicit WsClient(QObject *parent = nullptr);
    ~WsClient() override;
    Q_DISABLE_COPY(WsClient)

    void connectToServer();
    /// 应用层 {"type":"ping"}，不是 WebSocket 协议帧的 ping
    void sendPing();
    /// 未连接时只记 warning 并丢弃，不排队、不重试
    void sendJson(const QJsonObject &obj);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);
    void onSslErrors(const QList<QSslError> &errors);
    void onHeartbeatTimeout();
    void onReconnectTimeout();

private:
    QWebSocket *socket_;
    QTimer *heartbeatTimer_;
    QTimer *reconnectTimer_;
    // 最后一条收到的 alarm_id，仅用于重连后补 sync_request
    QString lastAlarmId_;
    // true = 调用方主动 stop()，此时断线不再重连
    bool stopping_;
};

#endif // WS_CLIENT_H
