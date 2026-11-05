// ============================================================================
// pad_http_server.cpp — v1 HTTP `/api/` 面的监听、keep-alive 会话与路由表
// ============================================================================
//
// 本文件里没有一条 v1 的业务规则（哪些字段必填、哪种状态冲突都在各路由文件里），
// 只有 socket 生命周期、报文框架与路由匹配。口径与取舍见 pad_http_server.h 文件头。
// ============================================================================
#include "pad_http_server.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QDebug>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace pad {

// ============================================================================
// PadHttpSession — 一条 TCP 连接
// ----------------------------------------------------------------------------
// 不带 Q_OBJECT：所有事件都靠 lambda 连接，所以本文件不需要 moc
// （PadHttpServer 自己带 Q_OBJECT，moc 只跑在头文件上）。
// 所有权：session 由 PadHttpServer 的 sessions_ 持有裸指针并负责 deleteLater，
// socket 是 session 的子对象。断线回调里会把自己从表里摘掉。
// ============================================================================
class PadHttpSession : public QObject
{
public:
    PadHttpSession(QTcpSocket *socket, PadHttpServer *server)
        : QObject(nullptr), socket_(socket), server_(server)
    {
        socket_->setParent(this);          // socket 随 session 一起析构
        clock_.start();                     // 单调时钟，专给空闲判定用（墙钟会跳）
        socket_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(socket_, &QTcpSocket::readyRead, this, [this]() { onReadyRead(); });
        connect(socket_, &QTcpSocket::disconnected, this, [this]() { onDisconnected(); });
        connect(socket_, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error),
                this, [this](QAbstractSocket::SocketError error) {
                    // 写回时对端已经挂了：keep-alive 的客户端什么时候关连接由它自己决定，
                    // 这是正常收场而不是故障。每次 qWarning 会把现场日志刷满
                    // （自测里 curl 连发三次就刷了三条）。
                    if (error == QAbstractSocket::RemoteHostClosedError) {
                        closeSocket();
                        return;
                    }
                    // 其余错误（连接被重置、超时）留着也没有意义，直接收掉
                    qWarning() << "PadHttpSession socket error:" << error
                               << socket_->errorString();
                    closeSocket();
                });
    }

    /// 会话建立后、第一个字节到达前设上限（每条连接一份解码器，所以只能在这里给）
    void decoderLimits(int maxHeadBytes, qint64 maxBodyBytes)
    {
        decoder_.setLimits(maxHeadBytes, maxBodyBytes);
    }

    /// 由服务端 stop() 调用：不发回复，直接关
    void forceClose() { closeSocket(); }

    bool socketOpen() const
    {
        return socket_ && socket_->state() != QAbstractSocket::UnconnectedState;
    }

    /// 空闲判定：距离上一次有任何字节进来的毫秒数
    qint64 idleMs() const { return clock_.elapsed(); }

    QString peerAddress() const
    {
        return socket_ ? socket_->peerAddress().toString() : QString();
    }

private:
    void onReadyRead()
    {
        lastActivity();
        const QByteArray incoming = socket_->readAll();
        if (incoming.isEmpty())
            return;
        if (!decoder_.feed(incoming)) {
            failAndClose();
            return;
        }
        // 一次 readyRead 可能带着好几个完整请求（keep-alive 上客户端不等回复就发下一个），
        // 所以循环取干；未收完的那半截留在 decoder 里等下一次 readyRead。
        while (decoder_.isComplete()) {
            padhttp::Request request;
            if (!decoder_.takeRequest(&request)) {
                // multipart 形状坏（takeRequest 里会置 Failed）——同一连接的剩余字节归属
                // 已经不可信，回一句 400 就必须关。
                failAndClose();
                return;
            }
            handleOne(request);
            if (!socketOpen())
                return;
        }
        // 取干之后还可能失败：takeRequest 会把缓冲里下一个（pipelining）请求接着解，
        // 那个解歪了就置 Failed，而已经交出的请求照常回复完了。留着连接只会继续报错。
        if (decoder_.isFailed()) {
            failAndClose();
            return;
        }
    }

    void handleOne(const padhttp::Request &request)
    {
        padhttp::Reply reply = server_->dispatch(request);
        // HEAD 不支持（Pad 不发）：路由表里根本没有 HEAD 条目，自然走到 404/2002。
        // keep-alive 的判据两条：对端明确要 close 就关；HTTP/1.0 默认短连接，也关。
        const bool wantsClose =
            request.header(QStringLiteral("connection")).compare(QStringLiteral("close"),
                                                                Qt::CaseInsensitive) == 0
            || request.version == QLatin1String("HTTP/1.0");
        const bool keepAlive = !reply.forceClose && !wantsClose;
        server_->noteRequest(request, reply);
        socket_->write(padhttp::serializeReply(reply, keepAlive));
        socket_->flush();
        if (!keepAlive)
            closeSocket();
    }

    void failAndClose()
    {
        const QString reason = decoder_.errorText();
        qWarning() << "PadHttpServer 收到畸形请求：" << reason;
        const padhttp::Reply reply = padhttp::replyBadRequest(
            QString::fromUtf8("请求报文不合法"), reason);
        // forceClose：解析已经不可恢复，留着连接只会在下一段字节上继续报错
        padhttp::Reply closing = reply;
        closing.forceClose = true;
        socket_->write(padhttp::serializeReply(closing, false));
        socket_->flush();
        closeSocket();
    }

    void lastActivity() { clock_.restart(); }

    void onDisconnected()
    {
        if (server_)
            server_->forgetSession(this);
        deleteLater();
    }

    void closeSocket()
    {
        if (!socket_)
            return;
        // 顺序要紧：先 Abort 会同步发 disconnected，那个回调里已经把本对象从表里摘掉
        // 并 deleteLater 了；这里只负责把状态置成"别再读写"。
        socket_->abort();
    }

    QTcpSocket *socket_ = nullptr;
    PadHttpServer *server_ = nullptr;
    padhttp::RequestDecoder decoder_;
    QElapsedTimer clock_;
};

// ============================================================================
// PadHttpServer
// ============================================================================

PadHttpServer &PadHttpServer::instance()
{
    // Meyers 单例：首次调用在**哪根线程**就归属哪根线程（装配处是 GUI 线程）。
    // 与 PadWsServer 同一个写法，两个服务端各自独立，谁也不碰谁的连接表。
    static PadHttpServer server;
    return server;
}

PadHttpServer::PadHttpServer(QObject *parent) : QObject(parent)
{
}

PadHttpServer::~PadHttpServer()
{
    stop();
}

bool PadHttpServer::start(const HttpServerConfig &cfg, QString *err)
{
    // 先验参数再判幂等（与 PadWsServer::start 同一条判序）：
    // 反过来写会让"已在好口上听着 + 又传进 port=0"被回成成功。
    if (cfg.port == 0) {
        if (err)
            *err = QString::fromUtf8("HTTP 服务端口是 0（配置里 pad_http.port 没给或给了非法值）");
        return false;
    }
    if (running())
        return true;

    cfg_ = cfg;
    server_ = new QTcpServer(this);
    server_->setMaxPendingConnections(16);   // Pad 只有几台，多余的排队连接一律是扫段口的
    connect(server_, &QTcpServer::newConnection, this, [this]() { onNewConnection(); });
    if (!server_->listen(QHostAddress::Any, cfg.port)) {
        const QString reason = server_->errorString();
        delete server_;
        server_ = nullptr;
        if (err) {
            *err = QString::fromUtf8("监听 %1 端口失败：%2")
                       .arg(cfg.port)
                       .arg(reason);
        }
        return false;
    }
    if (!sweepTimer_) {
        sweepTimer_ = new QTimer(this);
        connect(sweepTimer_, &QTimer::timeout, this, [this]() { sweepIdleSessions(); });
    }
    sweepTimer_->start(10000);
    return true;
}

void PadHttpServer::stop()
{
    if (sweepTimer_)
        sweepTimer_->stop();
    if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    // 先摘表再关：close 会同步触发 disconnected，那个回调里还要 removeOne 一次，
    // 表已经空了就找不到自己——所以这里把所有权接下来，逐个 deleteLater。
    const QList<PadHttpSession *> closing = sessions_;
    sessions_.clear();
    for (PadHttpSession *session : closing) {
        session->forceClose();
        session->deleteLater();
    }
}

bool PadHttpServer::running() const
{
    return server_ && server_->isListening();
}

quint16 PadHttpServer::port() const
{
    return running() ? static_cast<quint16>(server_->serverPort()) : 0;
}

int PadHttpServer::clientCount() const
{
    return sessions_.size();
}

quint64 PadHttpServer::requestCount() const
{
    return requestCount_;
}

void PadHttpServer::addRoute(const QString &method, const QString &pattern,
                             const HttpHandler &handler)
{
    Route route;
    route.method = method.toUpper();
    route.segments = pattern.split(QLatin1Char('/'));
    route.handler = handler;
    // 同 (method, pattern) 覆盖而不是并存：装配处每次 start 前都 clearRoutes()，
    // 但测试里会反复注册同一条件；并存的话"命中哪一条"取决于数组顺序，那是最难查的一种错。
    for (Route &existing : routes_) {
        if (existing.method == route.method && existing.segments == route.segments) {
            existing.handler = route.handler;
            return;
        }
    }
    routes_.append(route);
}

void PadHttpServer::clearRoutes()
{
    routes_.clear();
}

int PadHttpServer::routeCount() const
{
    return routes_.size();
}

void PadHttpServer::onNewConnection()
{
    while (server_ && server_->hasPendingConnections()) {
        QTcpSocket *socket = server_->nextPendingConnection();
        if (!socket)
            break;
        auto *session = new PadHttpSession(socket, this);
        session->decoderLimits(cfg_.maxHeadBytes, cfg_.maxBodyBytes);
        sessions_.append(session);
    }
}

void PadHttpServer::forgetSession(PadHttpSession *session)
{
    sessions_.removeOne(session);
}

void PadHttpServer::sweepIdleSessions()
{
    // 复制一份再遍历：closeSocket() 会同步走 disconnected → forgetSession → 改 sessions_，
    // 直接遍历原表会在改完之后继续用失效的下标。
    const QList<PadHttpSession *> snapshot = sessions_;
    const qint64 limit = qMax(1000, cfg_.idleTimeoutMs);
    for (PadHttpSession *session : snapshot) {
        if (!session->socketOpen())
            continue;
        if (session->idleMs() > limit) {
            session->forceClose();
            session->deleteLater();
        }
    }
}

padhttp::Reply PadHttpServer::dispatch(const padhttp::Request &request)
{
    // 路径规范化：只去掉**末尾**那一个 '/'（"/api/fences/" 与 "/api/fences" 同一条路由）。
    // 不去重复中间斜杠：那是另一个问题，静默改写会让日志里的路径和真实请求不一致。
    QString path = request.path;
    while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
        path.chop(1);
    const QStringList parts = path.split(QLatin1Char('/'));

    for (const Route &route : routes_) {
        if (route.method != request.method || route.segments.size() != parts.size())
            continue;
        QMap<QString, QString> params;
        bool matched = true;
        for (int i = 0; i < parts.size(); ++i) {
            const QString &want = route.segments.at(i);
            if (want.size() > 2 && want.startsWith(QLatin1Char('{'))
                && want.endsWith(QLatin1Char('}'))) {
                params.insert(want.mid(1, want.size() - 2), parts.at(i));
                continue;
            }
            if (want != parts.at(i)) {
                matched = false;
                break;
            }
        }
        if (!matched)
            continue;
        if (!route.handler) {
            // 注册了空处理函数是装配错误。回 500/3001 而不是静默 200：
            // 对端会把 200 当成"操作已生效"，那是最坏的失效方向。
            return padhttp::replyInternal(QString::fromUtf8("路由处理函数未装配"),
                                          request.method + QLatin1Char(' ') + path);
        }
        padhttp::Request routed = request;
        routed.path = path;
        routed.pathParams = params;
        return route.handler(routed);
    }

    // 未匹配：按 §2.4 那一档 404/2002（"对象不存在"）回。
    // detail 里带上方法+路径，现场对着 Pad 的报错一眼能看出是口配错还是路径配错。
    return padhttp::replyNotFound(QString::fromUtf8("这个接口本机没有实现"),
                                  request.method + QLatin1Char(' ') + path);
}

void PadHttpServer::noteRequest(const padhttp::Request &request, const padhttp::Reply &reply)
{
    ++requestCount_;
    // 只记写请求与失败：GET 是 Pad 进页面时拉的，成功也记会让调试页日志被翻页刷满。
    const bool isWrite = request.method != QLatin1String("GET");
    const bool failed = reply.status >= 400;
    if (!isWrite && !failed)
        return;
    QString text = QStringLiteral("%1 %2 → %3").arg(request.method, request.path)
                                                .arg(reply.status);
    if (failed) {
        // 失败才带信封原文（截到 200 字节）：现场对着这一句就能分清"路径配错"和
        // "字段没给全"，不用再抓包。成功响应可能几万字节，绝不进日志。
        text += QStringLiteral("  %1")
                    .arg(QString::fromUtf8(reply.body.left(200)).remove(QLatin1Char('\n')));
    }
    emit logMessage(failed ? QStringLiteral("warning") : QStringLiteral("system"), text);
}

}  // namespace pad
