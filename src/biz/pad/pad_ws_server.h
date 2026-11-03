#ifndef PAD_WS_SERVER_H
#define PAD_WS_SERVER_H

// ============================================================================
// pad_ws_server.h — Pad 接口 v1 的 WebSocket 服务端（本机是**服务端**）
// ============================================================================
//
// 与 src/biz/ws/wss_client 的区别就是方向：WsClient 是本机主动连出去（旧协议），
// v1 里 Pad 连本机。两套并存时旧的那条链路应当整块退役（见
// plan/代码审查_2026-11-02_协议对齐idge-pad-v1.md 冲突 B1），不要在本文件里加兼容分支。
//
// WS 只传 JSON 小消息，**不传图片/视频/APK/二进制帧**（v1 §1 硬性边界）；
// 证据只给 URL，由 HttpFileService 那台静态服务出字节。所以 baseUrl 为空时
// 证据数组就是空的——宁可不给地址，也不给一个对端取不到的地址。
//
// 消息面（v1 §3）：
//   收到 hello / ping / sync_request / alarm_ack / pong，其余回 error 2001（不崩溃）。
//   发出 hello_ack / pong / alarm / alarm_update / sync_result / error。
//   `alarm_ack` **不回复**；`error` 的 code/message 在信封层。
//
// 报警的两段推送：
//   1) AlarmManager::alarmGenerated 立刻转成 `alarm`（抓拍路径在这一步已经有了，
//      见 ffmpeg_video_decoder 的 deliverAlarms 顺序：先填 imagePath 再 storeAndNotify）；
//   2) 视频片段是"报警后 2 秒"才落盘的，而 v1 的 `alarm_update` 不带 evidence
//      （§13.2.3 只有 status/readAt/dispose/updatedAt）→ 片段到位后再发**一条完整 `alarm`**，
//      按 §3.8"重复下发同一报警是安全的、按 updatedAt 合并"。
//
// 线程：只在创建它的线程（GUI 线程）收发。AlarmManager 的信号来自解码线程/片段线程，
// AlarmRecord 已 Q_DECLARE_METATYPE，AutoConnection 会自动排队。
//
// 生命周期：start/stop 成对且**可重入**（与 HttpFileService 那种进程级一次性不同，
// QWebSocketServer 可以关掉了再开），端口被占返回 false 并给出原因。
// ============================================================================

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include "alarm_manager.h"   // AlarmRecord（信号槽参数类型）

class QWebSocket;
class QWebSocketServer;

namespace pad {

struct ServerConfig {
    quint16 port = 0;             ///< 0 = 非法，start 直接返回 false
    QString path;                 ///< 仅作日志显示；QWebSocketServer 不按路径过滤连接
    QString deviceId;             ///< hello_ack.deviceId，例 "EDGE-05-007"
    QString mediaBaseUrl;         ///< 证据 URL 前缀，空 = 不发地址
    QString mediaRootDir;         ///< 与 mediaBaseUrl 对应的那棵树（绝对路径）
    int syncLimit = 500;          ///< 一次 sync_result 最多带几条（v1 未定分页，先设上限）
};

class PadWsServer : public QObject
{
    Q_OBJECT
public:
    static PadWsServer &instance();

    // 起服务并订阅 AlarmManager。cfg 由装配处（frmMain）从 config.json 拼好传进来，
    // 本类不读 ConfigManager —— 那样才能不依赖配置层做离线自测。
    bool start(const ServerConfig &cfg, QString *err);
    void stop();

    bool running() const;
    int clientCount() const;
    quint16 port() const;

    // 手动推送（装配处/自测用；正常路径由 AlarmManager 信号驱动）
    void pushAlarm(const AlarmRecord &record);
    void pushAlarmUpdate(const AlarmRecord &record);

    // v1 §3.5 的 capabilities.alarm：push 恒真，evidenceUrl 取决于媒体服务是否可用
    QJsonObject helloAckData() const;

signals:
    // 与其它模块同一范式：category 固定 "pad"，由主窗口转接到设置页日志
    void logMessage(const QString &category, const QString &message);

private slots:
    void onNewConnection();
    void onClientDisconnected();
    void onTextMessage(const QString &text);
    void handleAlarmGenerated(const AlarmRecord &alarm);
    void handleVideoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                                 const QString &webPath);

private:
    explicit PadWsServer(QObject *parent = nullptr);
    ~PadWsServer() override;

    void sendText(QWebSocket *socket, const QByteArray &frame);
    void broadcast(const QByteArray &frame);
    void handleHello(QWebSocket *socket, const QJsonObject &data);
    void handleSyncRequest(QWebSocket *socket, const QJsonObject &data);

    QWebSocketServer *server_ = nullptr;
    QList<QWebSocket *> clients_;
    ServerConfig cfg_;
    bool alarmsWired_ = false;    ///< 只连一次 AlarmManager，避免 stop/start 后重复投递
};

} // namespace pad

#endif // PAD_WS_SERVER_H
