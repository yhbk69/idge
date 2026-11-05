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
    QString deviceId;             ///< hello_ack.deviceId，例 "EDGE-05-007"；**空 = 按本机派生**
                                  ///（pad::defaultDeviceId()，口径 A17：这个键 §13.1.3 规定必填）
    QString mediaBaseUrl;         ///< 证据 URL 前缀，空 = 不发地址
    QString mediaRootDir;         ///< 与 mediaBaseUrl 对应的那棵树（绝对路径）
    int syncLimit = 500;          ///< 一次 sync_result 最多带几条（v1 未定分页，先设上限）
                                  ///  <=0 时实现里按 500 兜底，不填比填 0 安全
};

/**
 * @brief Pad 接口 v1 的本机服务端（GUI 线程单例，Meyers 静态）
 *
 * 生命周期：instance() 首次调用（frmMain 装配处）决定它归属哪根线程，此后所有收发
 * 都只在那根线程上；本类内部不加锁，连接表 clients_ 也只在那根线程访问。
 * 与 AlarmManager 的关系：start() 里订阅它的三个信号（generated / statusChanged /
 * clipAttached），跨线程靠 AlarmRecord 的 Q_DECLARE_METATYPE + AutoConnection 排队；
 * alarmsWired_ 保证 stop→start 之后不会重复投递同一条报警。
 * 依赖方向：biz/pad → biz/alarm、biz/db、biz/ws(仅 mediaUrlFor)、base，不 include src/ui。
 */
class PadWsServer : public QObject
{
    Q_OBJECT
public:
    static PadWsServer &instance();

    // 起服务并订阅 AlarmManager。cfg 由装配处（frmMain）从 config.json 拼好传进来，
    // 本类不读 ConfigManager —— 那样才能不依赖配置层做离线自测。
    /**
     * @brief 监听 cfg.port 并接好 AlarmManager 的三个信号
     * @param err 可选出参，只在失败时被写（端口为 0 / listen 失败，中文原因）
     * @return false：port==0（参数错，优先于幂等判断返回）或 listen 失败（端口被占、
     *         或 1024 以下的特权端口在非 root 下起不来）；已在监听时**直接返回 true**，
     *         不重建 server_
     *
     * 判顺序的原因见 .cpp 里 start() 的注释：先验参数再判幂等，否则重复调用会把
     * 一个不能用的配置回成成功。
     */
    bool start(const ServerConfig &cfg, QString *err);
    /**
     * @brief 关闭监听并踢掉所有已连 Pad
     *
     * 可重复调用、可与 start() 交替（与 HttpFileService 那种进程级一次性不同）。
     * 注意与 AlarmManager 的信号连接**不解除**（alarmsWired_ 保持 true）：stop 之后
     * 报警回调仍会进来，但 pushAlarm/pushAlarmUpdate 开头有 running() 判据，会静默丢弃。
     */
    void stop();

    bool running() const;      ///< server_ 存在且正在监听（与"有没有 Pad 连着"无关）
    int clientCount() const;   ///< 当前已握手的连接数，仅用于日志与设置页显示
    quint16 port() const;      ///< 未监听时返回 0（不是 cfg_.port——让调用方看出"没起来"）

    // 手动推送（装配处/自测用；正常路径由 AlarmManager 信号驱动）
    void pushAlarm(const AlarmRecord &record);
    void pushAlarmUpdate(const AlarmRecord &record);

    // v1 §3.5 的 capabilities.alarm：push 恒真，evidenceUrl 取决于媒体服务是否可用
    QJsonObject helloAckData() const;

signals:
    // 与其它模块同一范式：category 固定 "pad"，由主窗口转接到设置页日志
    void logMessage(const QString &category, const QString &message);

private slots:
    /// 逐个取走 pending 连接并登记进 clients_（Origin 被拒的连接不会走到这里）
    void onNewConnection();
    /// 用 sender() 反查是哪根连接断了：断线必须 deleteLater，否则每根连接漏一只 QWebSocket
    void onClientDisconnected();
    /**
     * @brief v1 消息面的唯一入口：解析信封 → 按 type 分派
     *
     * 不是合法 JSON / 不是对象 / 未知 type 都回 error 2001（§3.3 要求"不能崩溃"），
     * 且都**不关连接**；alarm_ack 与 pong 只记日志不回包。
     */
    void onTextMessage(const QString &text);
    /// 报警产生（解码线程排队而来）→ 广播一条完整 alarm
    void handleAlarmGenerated(const AlarmRecord &alarm);
    /// UI 确认/误报 → 广播 alarm_update；acknowledgeAll() 不发这条（见 pad/README.md 口径 5）
    void handleAlarmStatusChanged(const AlarmRecord &alarm);
    /**
     * @brief 片段落盘（约报警 +2s）→ 从库里重读记录并广播一条 `alarm_update`（带 clipUrl）
     *
     * 流程（2026-11-05 现场指定）：报警当刻先发 `alarm`（信息 + 抓拍图，clipUrl=null），
     * 视频写完后**只发 `alarm_update` 把视频补上**，不再重发整条 `alarm`。
     * 必须从 DAO 重读：`updatedAt` 只在库里被 updateVideoPath 推前过，用内存记录会与
     * 上一条同 updatedAt，对端按"较新胜出"合并时会把这次更新丢掉。
     * videoPath/webPath 两个入参这里都不用，路径以 DAO 读回来的那份为准。
     * ⚠ §13.2.2 的字段表里还没有 `clipUrl`，这是现场指令先落的一项，规约待补。
     */
    void handleVideoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                                 const QString &webPath);

private:
    explicit PadWsServer(QObject *parent = nullptr);
    ~PadWsServer() override;

    /// 单播；socket 为空或已断开直接丢弃（调用方不检查返回值，WS 层没有重传）
    void sendText(QWebSocket *socket, const QByteArray &frame);
    /// 广播；无连接时静默丢弃——补漏是 sync_request 的职责，本机不做离线排队
    void broadcast(const QByteArray &frame);
    /// hello 的两级判据：必填缺失 → 2001 不关连接；protocolVersion != 1 → 1001 并关闭
    void handleHello(QWebSocket *socket, const QJsonObject &data);
    /// sync_request：taskId 空 → 2001；否则按 lastEventTime 游标查库并回一个 sync_result
    void handleSyncRequest(QWebSocket *socket, const QJsonObject &data);

    QWebSocketServer *server_ = nullptr;
    QList<QWebSocket *> clients_;   ///< 裸指针，所有权在本类：断开即 deleteLater
    ServerConfig cfg_;
    bool alarmsWired_ = false;    ///< 只连一次 AlarmManager，避免 stop/start 后重复投递
};

} // namespace pad

#endif // PAD_WS_SERVER_H
