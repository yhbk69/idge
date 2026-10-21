#ifndef WS_CLIENT_H
#define WS_CLIENT_H

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

    void start();
    void stop();

    void sendAck(const QString &alarmId);
    void sendSyncRequest(const QString &lastAlarmId);
    void sendGetStreams();
    void sendSetFence(int streamId, double x1, double y1, double x2, double y2);

    bool isConnected() const;
    QString lastAlarmId() const { return lastAlarmId_; }

signals:
    void alarmReceived(const QJsonObject &data);
    void streamsReceived(const QJsonArray &streams);
    void connected();
    void disconnected();
    void logMessage(const QString &category, const QString &message);

private:
    explicit WsClient(QObject *parent = nullptr);
    ~WsClient() override;
    Q_DISABLE_COPY(WsClient)

    void connectToServer();
    void sendPing();
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
    QString lastAlarmId_;
    bool stopping_;
};

#endif // WS_CLIENT_H
