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
#include <QWebSocketCorsAuthenticator>
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

// P2-3：来源白名单。v1 §2.3 免的是**鉴权**，不是免**来源检查**——
// 不判的话"现场平板的原生 App 能连"和"局域网里任何一个网页的 JS 都能连进来
// 实时收报警、按 taskId 拉历史"是同一个风险面，而报警里带人名、位置与截图地址。
// 判据：空 Origin 放行（原生客户端与 OkHttp 这类不发 Origin 的实现），
// android-app:// 前缀放行（Qt/WebView 侧 Android 原生容器发的 Origin），其余一律拒。
// 浏览器一定带 http(s):// Origin，所以这一条把"网页 JS 可连"彻底挡掉。
// 真需要放行别的来源时改这一个函数，并回来更新 README「网络暴露面」一节。
bool originAllowed(const QString &origin)
{
    if (origin.isEmpty()) {
        return true;
    }
    return origin.startsWith(QLatin1String("android-app://"));
}

} // namespace

// Meyers 单例：第一次调用（frmMain 装配处）决定归属线程，之后所有槽都在那根线程跑。
// 本类不加锁——clients_/cfg_ 只在创建线程访问，跨线程的 AlarmManager 信号靠排队投递进来。
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

/**
 * @brief 起监听并（首次）订阅 AlarmManager
 *
 * 三条 connect 用 Qt::AutoConnection 的默认语义（未显式指定连接类型）：
 * alarmGenerated 来自解码线程、videoClipAttached 来自片段线程，跨线程时自动排队到
 * 本对象所在的 GUI 线程，所以 handlers 里可以安全地碰 clients_ 与 DAO。
 * 前置条件：AlarmRecord 必须已 Q_DECLARE_METATYPE + 注册，否则跨线程投递在运行时告警并丢事件。
 *
 * 失败路径用 deleteLater 而不是 delete：server_ 是以 this 为 parent 建的，
 * 直接 delete 会在本次调用栈里就把正在被引用的对象拆掉；同时把 server_ 置空，
 * 免得 running() 还认这只已关掉的服务器。
 */
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
    // P2-3：来源白名单。Qt 5.12 的 QWebSocket 没有 requestOrigin()，唯一的钩子是
    // QWebSocketServer::originAuthenticationRequired——它只在客户端**带了非空 Origin** 时发，
    // 所以原生 App（OkHttp 那类不发 Origin）走不到这里、自然放行；浏览器一定发 http(s):// Origin，
    // 于是这一处正好把"局域网里任何网页的 JS 都能连进来实时收报警、按 taskId 拉历史"挡掉。
    // 不连接这个信号 = 所有 Origin 一律接受（本轮之前的实际行为）。
    connect(server_, &QWebSocketServer::originAuthenticationRequired, this,
            [this](QWebSocketCorsAuthenticator *authenticator) {
                const QString origin = authenticator->origin();
                const bool allow = originAllowed(origin);
                authenticator->setAllowed(allow);   // 不调这行默认也是 true，必须显式拒
                if (!allow) {
                    emit logMessage(QStringLiteral("pad"),
                                    QStringLiteral("拒绝连接：Origin=%1 不在白名单"
                                                   "（只放行空 Origin 与 android-app://*；"
                                                   "要改判据只改 originAllowed 一处）").arg(origin));
                }
            });

    if (!alarmsWired_) {
        // AutoConnection：alarmGenerated 来自解码线程、videoClipAttached 来自片段线程，
        // 跨线程时 Qt 自动排队到本对象所在的 GUI 线程，所以收发都只在一根线程上。
        connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
                this, &PadWsServer::handleAlarmGenerated);
        connect(&AlarmManager::instance(), &AlarmManager::videoClipAttached,
                this, &PadWsServer::handleVideoClipAttached);
        // 报告 P1-1：确认/误报后必须把这条的状态变化推出去。以前 pushAlarmUpdate()
        // 定义了却一个调用方都没有，板端点完「已确认」，Pad 那条还停在 PENDING，
        // 只能等下次重连靠 sync_result 拉平——违反 §3.7。
        connect(&AlarmManager::instance(), &AlarmManager::alarmStatusChanged,
                this, &PadWsServer::handleAlarmStatusChanged);
        alarmsWired_ = true;
    }

    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("Pad 服务端已监听 0.0.0.0:%1%2（明文 ws，零鉴权）")
                        .arg(cfg_.port).arg(cfg_.path));
    return true;
}

/**
 * @brief 关监听 + 关掉所有已连 Pad；与 start() 成对，可重复调用
 *
 * 不解除 AlarmManager 的三个 connect（alarmsWired_ 一直是 true）：stop 之后信号还会进来，
 * 但 pushAlarm/pushAlarmUpdate/handleVideoClipAttached 开头都有 running() 判据，一律静默丢。
 * 这样做的代价是"停掉的服务仍然在收报警回调"，换来的是再次 start() 不需要处理
 * 重复 connect（Qt 的 connect 会真的重复投递，得先 disconnect 才能防住）。
 *
 * NOTE(未验证的边界)：nextPendingConnection() 交出来的 QWebSocket **不是** server_ 的子对象，
 * 析构责任在本类。这里只 close + 清表，删除交给 onClientDisconnected；若某根连接因为异常
 * 没能发出 disconnected，这只 socket 就再也没人 deleteLater。
 */
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

// port() 在未监听时故意返回 0 而不是 cfg_.port：设置页拿它显示"当前监听端口"，
// 报出配置值会掩盖"服务其实没起来"这种状态。
quint16 PadWsServer::port() const
{
    return running() ? cfg_.port : 0;
}

/**
 * @brief 组装 hello_ack 的 data（serverTime / protocolVersion / deviceId / capabilities）
 *
 * 三个"诚实位"是评审定下来的口径，改之前先看 pad/README.md 第 4 条：
 *   - `detectionBoxes` 恒 false：v1 硬要求，本机不通过 WS 传检测框；
 *   - `evidenceUrl` 反映的只是 mediaBaseUrl 配了没有（那台静态服务在不在），
 *     不为了凑规约谎报 true——服务起不来时报 false 是"证据不可用"，不是协议违规（A17b）；
 *   - `deviceId` 配置为空时按 /etc/machine-id 派生（defaultDeviceId()，进程内只算一次），
 *     因为配置文件会被拷到别的板子，写死会撞车。
 * serverTime 走 wireNow()：v1 §2.2 要带时区，这里就是唯一补偏移的地方（落库格式不带）。
 */
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
    // §13.1.3 把 deviceId 列为必填，所以配置留空也要给一个（口径 A17：按 /etc/machine-id 派生）。
    // "空就整键不插"这一版实测过后果：Pad 按字段字典校验必填项，直接判这条连接不合规。
    data.insert(QStringLiteral("deviceId"),
                cfg_.deviceId.isEmpty() ? defaultDeviceId() : cfg_.deviceId);
    data.insert(QStringLiteral("capabilities"), capabilities);
    return data;
}

/**
 * @brief 把 server_ 的 pending 连接**全部**取干并登记
 *
 * 必须 while 循环：newConnection 每来一根发一次，若一次事件循环里同时到达多根
 * （Pad 断网重连时会连发），只 nextPendingConnection() 一次会让其余连接卡在 pending，
 * 握手完成却永远收不到 hello_ack。
 * 被 Origin 白名单拒掉的连接不会走到这里——Qt 5.12.8 上 originAuthenticationRequired
 * 在 newConnection 之前生效（板端实测，见 pad/README.md 第 6 条），所以 clients_ 里
 * 只有真正放行的。
 * 这三条 connect 的接收者一个是 this、一个是 socket 自己（错误回调），后者随
 * deleteLater 自动解除，不需要手工 disconnect。
 */
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

/**
 * @brief v1 消息面唯一入口：解析 → 按 type 分派
 *
 * 用 sender() 反查是哪根连接（三个槽都挂在 socket 上，没有带上下文的绑定），
 * qobject_cast 失败即返回——理论上不会发生，宁可不回包也不崩在陌生 sender 上。
 * 判失败的口径统一是"回 2001，不关连接"（§3.3 要求不能崩溃；关了反而让 Pad 走一遍
 * 重连+hello 流程，比补一个字段贵得多）。
 * 非 JSON 时 detail 只给 **len=字符数**、不给原文：现场码/对端实现可能是别系统载荷，
 * 打进日志等于把别人的数据抄进本机日志。
 */
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
    // P3-2：必填项缺失与版本不兼容是两件事，判据必须分开。
    // 以前缺 deviceId 会因为 toInt(0) 顺带落到 1001，于是 Pad 收到"协议版本不兼容"
    // 去查版本号，而真正的问题是它没带必填字段——报错方向反了比不报错更难查。
    // §2.3 不鉴权，所以 token 允许是**空串**，但键必须在（字典校验按必填项判）。
    QStringList missing;
    if (data.value(QStringLiteral("deviceId")).toString().trimmed().isEmpty()) {
        missing << QStringLiteral("deviceId");
    }
    if (!data.contains(QStringLiteral("token"))) {
        missing << QStringLiteral("token");
    }
    if (data.value(QStringLiteral("clientVersion")).toString().trimmed().isEmpty()) {
        missing << QStringLiteral("clientVersion");
    }
    if (!missing.isEmpty()) {
        sendText(socket, errorEnvelope(2001, QStringLiteral("hello 缺少必填项"),
                                       missing.join(QLatin1Char(','))));
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("hello 参数错误（%1），已回 2001：%2")
                            .arg(missing.join(QLatin1Char(',')), peerText(socket)));
        // 不关连接：参数补齐后重发一次 hello 就行，不必重建 TCP 再走一遍握手。
        return;
    }

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

/**
 * @brief 处理 sync_request：按游标 + taskId 从库里补漏，回一条 sync_result
 *
 * 三条实现约束（协议语义见 pad/README.md 与 plan/edge-pad-v1.md §3.7）：
 *   - **这条 SQL 是本机唯一按 update_time 取数的路径**，计划过滤下推进 SQL 而不是
 *     取一批再在 C++ 里筛（原因见下面那段注释，报告 §八 P1-2）；
 *   - 游标取的是行内 `max(update_time，空则 alarm_time)`：update_time 是"补漏游标"，
 *     老行可能为 NULL（见 src/biz/db/README.md 的列口径），退回报警时间才不会漏推；
 *   - 没有命中行时**原样回显请求里的 lastEventTime**（可为 null）：回一个比请求更早的
 *     值会让对端游标倒退，下次同步把已同步过的报警再拉一遍。
 *
 * 线程/性能：AlarmDAO 在这里是**同步查库**，跑在 GUI 线程上。syncLimit 上限 500 行
 * 加一次 JSON 组装；同层 SQL 在 35383 行的库上实测 28~34ms（src/biz/db/README.md），
 * 按这个量级一次同步可以接受。把上限调大或往这条查询加 JOIN 之前要重新量一次，
 * 别把 GUI 钉住。cfg_.syncLimit <= 0 时按 500 兜底（配置里写 0 不等于"不限"）。
 * 库没初始化（isDatabaseInitialized 为 false）时回**空数组 + 回显游标**而不是报错，
 * 对端读成"这段时间没有新报警"，比重试更合理。
 */
void PadWsServer::handleSyncRequest(QWebSocket *socket, const QJsonObject &data)
{
    const QString taskId = data.value(QStringLiteral("taskId")).toString().trimmed();
    const QString cursorWire = data.value(QStringLiteral("lastEventTime")).toString();

    // P3-3：§13.1.5 把 taskId 列为必填，以前"空 = 返回所有计划的报警"是联调期的方便，
    // 代价是把"Pad 还没进施工计划页"和"这台设备上所有计划的报警都给你"压成同一条响应——
    // 对端无法区分，现场就会出现"A 班次的平板同步到了 B 班次的报警"。
    // 现在按 §2.6 判 2001；要跨计划排查请直接用设置页/报警查询页，那条路不走本协议。
    if (taskId.isEmpty()) {
        sendText(socket, errorEnvelope(2001, QStringLiteral("sync_request 缺少必填项"),
                                       QStringLiteral("taskId")));
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("sync_request 被拒（taskId 为空）：%1").arg(peerText(socket)));
        return;
    }

    // §3.8：唯一过滤依据是 lastEventTime；lastAlarmId 只是辅助字段，本机不参与判断。
    // 空游标 = 全量。游标认不出来时同样按全量处理（internalTimeFromWire 返回空串）。
    const QString sinceInternal = internalTimeFromWire(cursorWire);

    QJsonArray alarms;
    QString lastAlarmId;
    QString maxInternal;
    if (AlarmManager::instance().isDatabaseInitialized()) {
        AlarmDAO dao;
        // 计划过滤下推进 SQL（报告 §八 P1-2）：以前是"取够 syncLimit 行再在 C++ 里筛"，
        // 库里几万行历史时这批全是**别的应用最早期**的报警，筛完剩 0 条 → 游标回显 null →
        // Pad 存 null → 下次还是全量，当前计划的报警永远同步不到，而且一条错误都不出。
        const QVector<AlarmRecord> rows = dao.queryUpdatedSince(
            sinceInternal, cfg_.syncLimit <= 0 ? 500 : cfg_.syncLimit, taskId);
        for (const AlarmRecord &record : rows) {
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

void PadWsServer::handleAlarmStatusChanged(const AlarmRecord &alarm)
{
    // §3.6：边缘侧「查看」只发 readAt，「处置/误报」要发 status + dispose。
    // **但必须发**——对端按 alarmId 就地合并，不发的那条在 Pad 上永远是旧状态。
    pushAlarmUpdate(alarm);
    emit logMessage(QStringLiteral("pad"),
                    QStringLiteral("已推送 alarm_update：%1 → %2（%3 路在线）")
                        .arg(alarm.id, wireStatus(alarm))
                        .arg(clients_.size()));
}

/**
 * @brief 片段落盘回调：逐条按 id 从库里重读，再广播一条 `alarm_update`（带 clipUrl）
 * @param alarmIds 空列表直接返回；没连着 Pad（running() 为 false）也直接返回，不做排队
 * @param videoPath / webPath 刻意不用（见下面 Q_UNUSED）：路径以库里读回来的那份为准，
 *        信号里的裸流路径只是"刚写到哪里"，与 alarms.video_path 的入库口径不保证相同
 *
 * **流程（2026-11-05 现场指定）**：报警当刻先发 `alarm`（信息 + 抓拍图，clipUrl=null），
 * 片段写完整之后**只发 `alarm_update` 把视频补上**，不再重发整条 `alarm`。
 * 对端按 alarmId + updatedAt 就地合并，其余字段不动。
 *
 * 为什么要多一次 queryById：对端合并只看 updatedAt，而 update_time 只在**库里**被
 * AlarmDAO::updateVideoPath 推前过；直接用内存记录会发出与上一条同 updatedAt 的更新，
 * 对端按"较新胜出"会把这次更新丢掉。
 * 代价是每个 id 一次单行查询（N 通常 1~2 条，走主键，量级可忽略）。
 * 记录已被清理/尚未落库时 record.id 为空 → 跳过该条，不广播半截更新。
 */
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
        pushAlarmUpdate(record);
        // 日志不复述"带 clipUrl"：mp4 万一没写成，这条 update 就不带那个键（宁缺勿假），
        // 写死一句话反而让现场以为视频已经上去了。
        emit logMessage(QStringLiteral("pad"),
                        QStringLiteral("片段落盘，已推送 alarm_update：%1").arg(record.id));
    }
}

// 两个 push 都以 running() 打底：stop() 不解除 AlarmManager 的信号连接（见 start 注释），
// 所以停服后仍然会进到这里，必须在这里丢掉，否则会往已经关掉的 socket 上写。
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
    broadcast(envelope(QStringLiteral("alarm_update"),
                       alarmUpdateJson(record, cfg_.mediaBaseUrl, cfg_.mediaRootDir)));
}

// 单播的尽力而为：state 不是 Connected 就静默丢掉，不记日志也不返错——调用方都是
// "事件已经发生、错过就错过"的语义（Pad 靠 sync_request 拉平），这里返错只会诱导
// 上层去做本机没有的重试队列。
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
