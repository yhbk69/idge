#ifndef PAD_HTTP_SERVER_H
#define PAD_HTTP_SERVER_H

// ============================================================================
// pad_http_server.h — Pad 接口 v1 的 HTTP `/api/` 面服务端（本机是服务端）
// ============================================================================
//
// 与同目录两份文件的关系：
//   pad_http_message.{h,cpp}  = 报文形状（解析 + §2.4 信封），不碰 socket；
//   本文件/实现              = 监听、keep-alive 会话、路由表，不碰任何协议业务规则；
//   pad_http_alarms / _fences / _streams / _device = §4~§7 的路由实现，只依赖 Request/Reply。
// 这么切的目的：路由能脱离网络做自测（直接构造 Request），传输能脱离协议做自测
// （喂一段字节验解析），两边都不必起对方。
//
// 为什么自己用 QTcpServer 而不挂到内嵌 ZLM 的 http 服务上（三条，都是实测/头文件核对过的）：
//   1) **端口要能运行期换**。ZLM 那台是进程级一次（mk_env_init 之后 stop 了不能再 start，
//      见 src/media/server/http_file_service.h 文件头第 1 条），调试页「保存并应用」
//      换口就必须重启进程；QTcpServer 与 PadWsServer 一样可以关了再开，行为口径统一。
//   2) **`mk_parser` 不替我们解 multipart**。3rdparty/zlmediakit/include/mk_events_objects.h
//      里与请求体相关的只有 `mk_parser_get_content(ctx, size_t*)`，
//      而 §4.2 的处置接口是 `multipart/form-data` + 0~5 张照片，part 边界照样得自己切；
//      换不来任何东西，却要把 `mk_*` 引进 biz 层（那条"全工程只有 http_file_service 能 include
//      mk_*.h"的规矩就破了）。
//   3) **静态文件不归这里**。§9 那些证据/围栏画面的字节仍由 8090 那台文件服务出
//      （它的 Range、目录穿越、404 行为是逐条 curl 核过的，见 src/media/server/README.md）。
//      本服务端只管 `/api/`，URL 里的 host:port 用媒体服务那台的实际地址拼
//      （口径见 pad_http_alarms.cpp 的 baseUrl 注释）。
//
// 线程：只在创建它的线程（GUI 线程）收发，与 PadWsServer 同一条规矩。
//   所有 handler 都在这根线程上**同步**执行，所以路由里做的库写、图片落盘都在这根线程上
//   —— 报警落库那套已经有专门的后台线程（AlarmDbWriter），本层不碰它；
//   本层的写操作只有 Pad 点一次才来一条的量级。
//
// 安全口径（必须知道再开）：明文 HTTP + 零鉴权，局域网内任何设备都能
//   改报警状态（处置/误报/已读）、增删改围栏。§2.3 写的是"默认不做 Token 鉴权"，
//   本机照此实现，但这是**写**面，比 WS 那条只读面更敞，
//   所以总开关与端口都摆在调试页上，关掉就是彻底不 bind。
// ============================================================================

#include <QList>
#include <QObject>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <functional>

#include "pad_http_message.h"

class QTcpServer;
class QTimer;

namespace pad {

class PadHttpSession;   // 定义在 .cpp 里（不带 Q_OBJECT，所以本文件之外没有它的形状）

/// 一次监听的参数（由装配处 frmMain 从 config.json 拼好传进来，本类不读 ConfigManager，
/// 这样离线自测能自己起一台在 127.0.0.1 的随机口上）
struct HttpServerConfig {
    quint16 port = 0;                 ///< 0 = 非法，start 直接返回 false
    QString apiPrefix = QStringLiteral("/api");   ///< 仅日志显示；路由本身已经带 /api
    qint64 maxBodyBytes = 26LL * 1024 * 1024;     ///< 正文上限：§4.2 五张 5MB + JSON 余量
    int maxHeadBytes = 16384;         ///< 请求行+头上限
    int idleTimeoutMs = 60000;        ///< 空闲连接回收（Pad 的 OkHttp 连接池会长期挂着）
    QString deviceId;                 ///< 仅日志，例 "EDGE-05-007"
};

/// 路由处理函数：拿一个已解析的 Request，回一个 Reply。**不得阻塞**（在 GUI 线程上跑）
using HttpHandler = std::function<padhttp::Reply(const padhttp::Request &)>;

/**
 * @brief v1 HTTP 面的监听与路由（GUI 线程单例，Meyers 静态）
 *
 * 生命周期：start()/stop() 成对且**可重入**（同 PadWsServer）。
 * start() 在已监听时直接返回 true、不换成新端口 —— 换口必须先 stop()。
 * 路由表在 start 前后都能改（装配处就是先 clearRoutes()+addRoute() 再 start）。
 */
class PadHttpServer : public QObject
{
    Q_OBJECT
public:
    static PadHttpServer &instance();

    /**
     * @brief 监听 cfg.port
     * @param err 可选出参，只在失败时被写（中文原因）
     * @return false = port 为 0，或 listen 失败（端口被占 / 1024 以下特权口）
     *
     * 与 PadWsServer::start 同一判序：**先验参数再判幂等**，
     * 否则"已经在一个好口上听着 + 又传进来一个 port=0"会被回成成功。
     */
    bool start(const HttpServerConfig &cfg, QString *err);
    /// 关监听并踢掉所有已连客户端（不注销路由表，重新 start 后原路由仍在）
    void stop();

    bool running() const;
    quint16 port() const;             ///< 未监听时返回 0（不是 cfg 里那个）
    int clientCount() const;          ///< 当前挂着的连接数（含还没发请求的）
    quint64 requestCount() const;     ///< 本进程累计处理完的请求数，日志与自测用

    /// 注册一条路由。pattern 形如 `/api/alarms/{alarmId}/dispose`，
    /// `{name}` 抓一段（不含 '/'），结果进 Request::pathParams。
    /// 同一 (method, pattern) 重复注册**后者覆盖前者**：装配处可以安全地重建整张表。
    void addRoute(const QString &method, const QString &pattern, const HttpHandler &handler);
    void clearRoutes();
    int routeCount() const;

signals:
    /// 与其它模块同一范式：category 固定 "pad-http"，由主窗口转接到调试页日志。
    /// GET 成功不落这条（否则 Pad 每进一次页面就刷一排），POST 与所有失败都落。
    void logMessage(const QString &category, const QString &message);

private:
    explicit PadHttpServer(QObject *parent = nullptr);
    ~PadHttpServer() override;
    Q_DISABLE_COPY(PadHttpServer)

    friend class PadHttpSession;

    /// 会话把一条已解析的请求交上来，本函数查路由并回一个 Reply。
    /// 查不到 → 404/2002；handler 为空 → 500/3001（装配出错，不静默回 200）。
    padhttp::Reply dispatch(const padhttp::Request &request);

    /// 会话回收：由 10s 一跳的清扫定时器调用，关掉空闲超时的连接
    void sweepIdleSessions();

    /// 取走 pending 连接并各建一个会话（Origin/路径都不筛，与 PadWsServer 同一口径）
    void onNewConnection();

    /// 会话自己断了（disconnected 回调）：从表里摘掉。对象由会话那边 deleteLater。
    void forgetSession(PadHttpSession *session);

    /// 每条请求处理完记账 + 按需落日志（只记写请求与失败，见信号注释）
    void noteRequest(const padhttp::Request &request, const padhttp::Reply &reply);

    struct Route {
        QString method;
        QStringList segments;      ///< pattern 按 '/' 切开（含开头那个空段）
        HttpHandler handler;
    };

    QTcpServer *server_ = nullptr;
    QVector<Route> routes_;
    QList<PadHttpSession *> sessions_;
    HttpServerConfig cfg_;
    QTimer *sweepTimer_ = nullptr;
    quint64 requestCount_ = 0;
};

}  // namespace pad

#endif  // PAD_HTTP_SERVER_H
