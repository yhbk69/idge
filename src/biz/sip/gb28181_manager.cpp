/**
 * @file gb28181_manager.cpp
 * @brief GB28181 设备侧信令实现（注册/心跳/目录查询/INVITE 点流/BYE）
 *
 * 线程模型：一条 std::thread 跑 eventLoop()，200ms 一轮 select()，同一轮里推进
 * 所有流的注册续期与心跳；SIP 只有一个 UDP socket，全部流共用。媒体不在这里——
 * INVITE 到达才 startMedia()，每路由 Gb28181MediaSource 自己起一条工作线程，
 * 编码出来的 PS 包经 PsMuxer 回调直接 U 到平台，不经过本文件的事件循环。
 *
 * 报文拼装全部是手写字符串（没有 SIP 库），因此有几处必须知道的取舍：
 *   - MANSCDP 的 XML 声明写的是 encoding="GB2312"，但 Content-Length 按 UTF-8
 *     字节数算、正文也按 UTF-8 发出（toStdString）。字段值全是 ASCII 时两者等价、
 *     平台能解；一旦 DeviceName/serialNumber 含中文，声明与实发不一致，平台侧
 *     按 GB2312 解会成乱码。要真接国标平台请先验证这一点。
 *   - Via 头恒写 SIP/2.0/UDP，与配置 platform.transport 无关（该键只打一条告警）。
 *   - serverHost / advertiseHost 只认 IPv4 字面量：inet_pton 失败就直接 return，
 *     注册和心跳会"静默不发"，日志里看不到错误——填域名是最容易踩的坑。
 *
 * 会话状态：calls_ 只映射 INVITE 建立的媒体会话，call-id 为键，仅事件线程访问；
 * 媒体源启动后的头 5 秒 PTS 会按墙钟 sleep_until 逐帧节流（防止回放型源瞬间灌爆
 * 平台收流缓冲），5 秒之后不再限速。BYE 只停媒体、不删流配置，平台可以再次 INVITE。
 *
 * 与其它模块的关系：本模块自己开 v4l2/RTSP，不和监控页的 FFmpegVideoDecoder、
 * 智能识别页、摄像头预览做任何互斥。同一路设备被两处同时打开会有一方失败。
 */
#include "gb28181_manager.h"
#include "gb28181_ssrc.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QStringList>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// SIP 头解析：按 \r\n 切行、冒号前是头名且大小写不敏感（RFC3261），只取第一条匹配。
// Via/From/To 在这里各取一次就够——本模块不转发也不级联，原样拼回响应即可。
// 返回空串 = 报文里没有这个头。调用方都不判存在性，缺头时会拼出 `Via: \r\n` 这样的空值响应，
// TODO: 真要接异构平台前应在这里补一条"必需头缺失"的告警，否则现场只能从抓包看出为什么被丢。
QString headerValue(const QString &message, const QString &name) {
    const QStringList lines = message.split(QStringLiteral("\r\n"));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon > 0 && line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return line.mid(colon + 1).trimmed();
    }
    return {};
}

QString callId(const QString &message) { return headerValue(message, QStringLiteral("Call-ID")); }

// 状态行形如 "SIP/2.0 401 Unauthorized"；解析不出返回 0（0 不是合法 SIP 状态码，调用方据此忽略）
int statusCode(const QString &message) {
    const QString first = message.section(QStringLiteral("\r\n"), 0, 0);
    const QStringList fields = first.split(QLatin1Char(' '), QString::SkipEmptyParts);
    return fields.size() > 1 ? fields.at(1).toInt() : 0;
}

// 请求行的第一个 token 即方法名；响应报文走到这里会得到 "SIP/2.0"，靠 handleDatagram 的分派挡住
QString requestMethod(const QString &message) {
    const QString first = message.section(QStringLiteral("\r\n"), 0, 0);
    return first.section(QLatin1Char(' '), 0, 0).trimmed();
}

// 正文 = 头部空行之后的全部内容。刻意**不**按 Content-Length 截断：一次 recvfrom 就是一个
// UDP 数据报，SIP over UDP 不粘包（README「SIP 只有一个 socket」一节），截断反而要把
// 解析头再写一遍。NOTE: 若将来支持 SIP over TCP，这里必须改成按 Content-Length 取。
QString body(const QString &message) {
    const int separator = message.indexOf(QStringLiteral("\r\n\r\n"));
    return separator < 0 ? QString() : message.mid(separator + 4);
}

// 只匹配带引号的摘要字段（RFC2617 的 WWW-Authenticate 挑战里 realm/nonce/qop 都带引号）。
// 失效方向：平台把 qop 写成不带引号的裸 token 时这里取不到，qop 为空 → 走"无 qop"的
// 摘要算法分支，平台会再回一次 401，表现为"注册永远不成功"。遇到这种平台先抓包看挑战头。
QString digestField(const QString &header, const QString &field) {
    const QRegularExpression expression(QStringLiteral("%1\\s*=\\s*\\\"([^\\\"]+)\\\"").arg(field));
    const auto match = expression.match(header);
    return match.hasMatch() ? match.captured(1) : QString();
}

// 摘要算法固定 MD5：GB28181 沿用 RFC2617，平台侧普遍只实现 MD5，写 SHA-256 会直接解不出
QString md5(const QString &value) {
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Md5).toHex());
}

// 只转义 & < > 三个：所有值都作为**元素文本**发出（<DeviceName>x</DeviceName>），不进属性值，
// 所以不必转义引号。顺序不能调换——先转 & 再转 <，反过来会把 &lt; 二次转义成 &amp;lt;。
QString xmlEscape(QString value) {
    return value.replace(QLatin1Char('&'), QStringLiteral("&amp;")).replace(QLatin1Char('<'), QStringLiteral("&lt;"))
        .replace(QLatin1Char('>'), QStringLiteral("&gt;"));
}

// SDP 按行取值：行首精确匹配 prefix，返回该行剩余部分。对端可能用 \r\n 换行，
// 行尾那个 \r 由 trimmed() 一并清掉——少这一步 port 会解析成 0。
QString sdpField(const QString &sdp, const QString &prefix) {
    const QStringList lines = sdp.split(QLatin1Char('\n'));
    for (const QString &line : lines) if (line.startsWith(prefix)) return line.mid(prefix.size()).trimmed();
    return {};
}

// MANSCDP 单层标签取值（<CmdType>DeviceInfo</CmdType>）。<base>[^<]*</base> 的形状决定了
// 取不到嵌套标签、CDATA 和跨行值；国标这几个查询字段都是扁平单值，够用。
QString xmlField(const QString &xml, const QString &name) {
    const QRegularExpression expression(QStringLiteral("<%1>([^<]*)</%1>").arg(name));
    const auto match = expression.match(xml);
    return match.hasMatch() ? match.captured(1) : QString();
}
}

Gb28181Manager::Gb28181Manager() = default;

Gb28181Manager::~Gb28181Manager() { stop(); }

/**
 * @brief 读配置 → 开 SIP socket → 每条 enabled 流建一个 Runtime → 起事件线程
 * @param configPath data/config.json 路径，读不到或校验不过只是返回 false（不抛不改全局状态）
 * @return false 三种情况：配置校验失败 / socket 开不起来 / 一条 enabled 流都没有
 *
 * 幂等：已在运行（running_ 为真）时直接返回 true，不会重复起线程。
 * 前置：只能在 main() 里调一次，之后想再 start() 要重新给 configPath（stop() 已把 streams_ 清空）。
 *
 * 时序约束：streams_ 必须在 running_ = true 之前全部建好——事件线程从这一行起就会遍历
 * streams_ 与 calls_，两边没有加锁，靠"先建完再起线程 + stop() 先 join 再 clear"这个顺序保证
 * 单线程访问。Runtime 的生命周期由 streams_ 持有，muxer 回调（媒体线程）捕获的是裸指针，
 * 所以 stop() 必须先 media->stop()（join 掉媒体线程）再析构 Runtime，顺序反了就是 use-after-free。
 */
bool Gb28181Manager::start(const QString &configPath) {
    if (running_.load()) return true;
    QString error;
    if (!Gb28181Config::load(configPath, &config_, &error)) {
        qWarning() << "GB28181 disabled:" << error;
        return false;
    }
    if (!openSocket()) return false;
    for (const auto &streamConfig : config_.streams) {
        if (!streamConfig.enabled) continue;
        auto runtime = std::make_unique<Runtime>();
        runtime->config = streamConfig;
        runtime->media = std::make_unique<Gb28181MediaSource>(streamConfig);
        runtime->rtp = std::make_unique<Gb28181RtpSender>();
        Runtime *raw = runtime.get();
        // 回调跑在 Gb28181MediaSource 的工作线程上（不是事件线程），所以这里只碰 Runtime 的
        // 计数与 rtp，不碰 streams_/calls_。日志刻意按"首条 + 每 100 条"采样：RTP 每帧多包，
        // 目的端口不可达时逐包打印会把板子串口刷死，也看不出真实故障率。
        runtime->muxer = std::make_unique<Gb28181PsMuxer>([this, raw](const std::vector<uint8_t> &ps, uint32_t pts) {
            if (!raw->rtp) return;
            ++raw->psFrames;
            const bool sent = raw->rtp->sendPs(ps, pts);
            if (!sent) {
                ++raw->rtpSendErrors;
                const int socketError = raw->rtp->lastError();
                if (raw->rtpSendErrors == 1 || (raw->rtpSendErrors % 100) == 0)
                    qWarning() << "GB28181 RTP send failed" << raw->config.deviceCode
                               << "errors=" << raw->rtpSendErrors << "psBytes=" << ps.size()
                               << "errno=" << socketError << "reason=" << QString::fromLocal8Bit(strerror(socketError))
                               << "destination=" << raw->remoteHost << raw->remotePort;
                if (raw->transport == QStringLiteral("tcp") && raw->media) {
                    qWarning() << "GB28181 TCP media session failed; stopping source, waiting for next INVITE"
                               << raw->config.deviceCode << "call=" << raw->callId;
                    raw->media->requestStop();
                }
            } else if (raw->psFrames == 1 || (raw->psFrames % 100) == 0) {
                qInfo() << "GB28181 RTP PS sent" << raw->config.deviceCode
                        << "frames=" << raw->psFrames << "psBytes=" << ps.size()
                        << "pts90k=" << pts;
            }
        });
        raw->nextRegisterMs = 0;
        streams_.push_back(std::move(runtime));
    }
    if (streams_.empty()) {
        closeSocket();
        qWarning() << "GB28181 configuration contains no enabled streams";
        return false;
    }
    running_ = true;
    thread_ = std::thread(&Gb28181Manager::eventLoop, this);
    return true;
}

/**
 * @brief 停事件线程 → 关 socket → 逐路停媒体 → 清空 streams_ / calls_
 *
 * 幂等，但两条分支不等价：exchange 返回旧值为 false（没启动过 / 已停过）时只做兜底
 * 清理并直接返回——不去 join 一个已经结束的线程。真正停一次的顺序是
 * join 事件线程 → closeSocket → stopMedia → clear，**不能调换**：
 * 先 clear 会让事件线程正在遍历的 streams_ 变成悬垂 vector，先 stopMedia 则媒体线程
 * 还在往 muxer 回调里送包（回调持有裸指针）。
 * 后置条件：running() 为 false，再次 start() 需要重新给 configPath（配置已随 streams_ 销毁）。
 */
void Gb28181Manager::stop() {
    if (!running_.exchange(false)) {
        for (auto &runtime : streams_) stopMedia(*runtime);
        closeSocket();
        return;
    }
    if (thread_.joinable()) thread_.join();
    closeSocket();
    for (auto &runtime : streams_) stopMedia(*runtime);
    streams_.clear();
    calls_.clear();
}

/**
 * @brief 建立本机 SIP UDP socket 并绑定 localHost:localSipPort
 * @return false 时已经自己把 socket 关掉，调用方（start）不必再 closeSocket
 *
 * localHost 为空或字面量 "0.0.0.0" 都按 INADDR_ANY 绑（advertiseHost 另有非通配校验，
 * 见 gb28181_config.cpp）。非 IPv4 字面量在这里是**有日志的**——与 sendRegister/
 * sendHeartbeat 里那两个静默 return 不同，别把两处当成同一个失败口径。
 *
 * SO_REUSEADDR 的返回值刻意不检查：它失败只会让"重启时旧 TIME_WAIT 端口占着"这一种
 * 场景 bind 不上，而 bind 失败本来就走下面那条 qWarning；同一台机器上跑第二个实例
 * 抢端口时这个选项也不保证负载均衡（见 sip/README.md 注意事项）。
 */
bool Gb28181Manager::openSocket() {
    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) { qWarning() << "GB28181 cannot create SIP socket:" << strerror(errno); return false; }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(static_cast<uint16_t>(config_.platform.localSipPort));
    const QByteArray host = config_.platform.localHost.toLocal8Bit();
    if (host.isEmpty() || config_.platform.localHost == QStringLiteral("0.0.0.0")) local.sin_addr.s_addr = INADDR_ANY;
    else if (::inet_pton(AF_INET, host.constData(), &local.sin_addr) != 1) {
        qWarning() << "GB28181 invalid SIP bind address" << config_.platform.localHost;
        closeSocket();
        return false;
    }
    int reuse = 1;
    ::setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (::bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) {
        qWarning() << "GB28181 cannot bind SIP port" << config_.platform.localSipPort << strerror(errno);
        closeSocket();
        return false;
    }
    if (config_.platform.transport != QStringLiteral("udp"))
        qWarning() << "GB28181 SIP transport" << config_.platform.transport << "is not available in this build; using UDP";
    return true;
}

void Gb28181Manager::closeSocket() {
    if (socket_ >= 0) { ::shutdown(socket_, SHUT_RDWR); ::close(socket_); socket_ = -1; }
}

// 写进 Via/Contact/Call-ID 域的本机对外地址。原样返回 advertiseHost，**不做** 0.0.0.0
// 兜底：那个值在配置校验阶段就已经被拒（gb28181_config.cpp 的 advertiseHost 检查），
// 能走到这里说明它可用。
QString Gb28181Manager::advertisedAddress() const {
    return config_.platform.advertiseHost;
}

// Call-ID 三段式：设备编码 + 墙钟毫秒 + 进程内自增计数。ms 单独不够——同一毫秒内
// handleQuery 可能给多条命令各建一个事务，callCounter_ 就是补这个洞的。
// 只在事件线程调用（含 start() 首轮），所以计数器不必原子。
QString Gb28181Manager::newCallId(const QString &deviceCode) {
    return QStringLiteral("%1-%2-%3@%4").arg(deviceCode).arg(QDateTime::currentMSecsSinceEpoch()).arg(callCounter_++).arg(advertisedAddress());
}

/**
 * @brief 组装 RFC2617 的 Authorization 头值（GB28181 注册用）
 * @param runtime 必须已带 nonce（401/407 解出来的），否则返回空串
 * @param method  目前只有 "REGISTER"
 * @param uri     与请求行完全一致的 Request-URI，写错一位摘要就对不上
 * @return 完整头值；nonce 为空时是空串
 *
 * 两条分支的算法不同，选哪条由**平台挑战**决定而不是本机配置：
 *   - 挑战带 qop → HA3 = MD5(A1:nonce:nc:cnonce:qop:A2)，头里附 qop/nc/cnonce
 *   - 挑战不带 qop → 经典 MD5(A1:nonce:A2)
 * A1 用 deviceCode 当用户名（国标口径，不是配置里的任何"账号"），realm 原样用挑战里的值。
 * 口令优先级：stream.password → platform.password（与 gb28181_config.cpp 的解析口径一致）。
 *
 * 取舍：nc 恒为 "00000001"、cnonce 取 MD5(deviceCode+nonce) 前 16 字符而不是随机数。
 * 因为每次续注都会拿到新 nonce、旧 nc 不会跨 nonce 复用，重放窗口本就等于 nonce 有效期；
 * 换成真随机只会让"同一次挑战重发"这种排障场景对不上日志。平台若严格校验 nc 递增才会踩到。
 */
QString Gb28181Manager::digestAuthorization(const Runtime &runtime, const QString &method, const QString &uri) const {
    if (runtime.nonce.isEmpty()) return {};
    const QString realm = runtime.realm;
    const QString password = runtime.config.password.isEmpty() ? config_.platform.password : runtime.config.password;
    const QString a1 = md5(runtime.config.deviceCode + QLatin1Char(':') + realm + QLatin1Char(':') + password);
    const QString a2 = md5(method + QLatin1Char(':') + uri);
    if (!runtime.qop.isEmpty()) {
        const QString nc = QStringLiteral("00000001");
        const QString response = md5(a1 + QLatin1Char(':') + runtime.nonce + QLatin1Char(':') + nc + QLatin1Char(':') + runtime.cnonce + QLatin1Char(':') + runtime.qop + QLatin1Char(':') + a2);
        return QStringLiteral("Digest username=\"%1\", realm=\"%2\", nonce=\"%3\", uri=\"%4\", response=\"%5\", algorithm=MD5, qop=%6, nc=%7, cnonce=\"%8\"")
            .arg(runtime.config.deviceCode, realm, runtime.nonce, uri, response, runtime.qop, nc, runtime.cnonce);
    }
    const QString response = md5(a1 + QLatin1Char(':') + runtime.nonce + QLatin1Char(':') + a2);
    return QStringLiteral("Digest username=\"%1\", realm=\"%2\", nonce=\"%3\", uri=\"%4\", response=\"%5\", algorithm=MD5")
        .arg(runtime.config.deviceCode, realm, runtime.nonce, uri, response);
}

// 唯一的 SIP 发包出口。sendto 的返回值刻意不检查也不重试：UDP 无链路反馈，
// 丢包由 SIP 事务自身的周期性重发兜底（注册看 nextRegisterMs、心跳看 nextHeartbeatMs，
// 对平台请求的响应则由平台超时重发）。MSG_NOSIGNAL 对 UDP 其实不生效，留着与 TCP 路径统一。
void Gb28181Manager::sendMessage(const std::string &message, const sockaddr_in &peer) {
    if (socket_ >= 0) ::sendto(socket_, message.data(), message.size(), MSG_NOSIGNAL,
                                reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
}

/**
 * @brief 发一条 REGISTER（首轮裸发，401 之后带 Digest 重发）
 * @param withAuth true 时附 Authorization 头；调用方（eventLoop）用 nonce 是否为空决定，
 *                 传 true 但 nonce 已被清空会发出空的 Authorization 头 → 平台再回 401，不会崩
 *
 * 副作用：占用/推进 runtime.registerCallId、cseq、nextRegisterMs。
 * Call-ID 复用同一个（注册事务必须稳定，换 Call-ID 等于新设备）；branch 每次新生成。
 *
 * 失败场景：serverHost 不是 IPv4 字面量时 **静默 return**（inet_pton != 1，一行日志都没有），
 * 且 nextRegisterMs 不会被推进 → 每轮 select（200ms）都重发一次，现象是"设备永远不在线"
 * 而不是一条错误。这是 sip/README.md 里点名的头号坑，排查先 grep dropped、
 * 再把域名换成 IP。
 */
void Gb28181Manager::sendRegister(Runtime &runtime, bool withAuth) {
    const QString domain = config_.platform.serverDomain.isEmpty() ? config_.platform.serverHost : config_.platform.serverDomain;
    const QString uri = QStringLiteral("sip:%1:%2").arg(config_.platform.serverHost).arg(config_.platform.serverPort);
    const QString from = QStringLiteral("<sip:%1@%2>").arg(runtime.config.deviceCode, domain);
    const QString call = runtime.registerCallId.isEmpty() ? newCallId(runtime.config.deviceCode) : runtime.registerCallId;
    runtime.registerCallId = call;
    const QString branch = QStringLiteral("z9hG4bK-%1").arg(QDateTime::currentMSecsSinceEpoch());
    // 手写整段报文，字段顺序照 RFC3261 常见实现。两个固定值有出处：
    //   - "z9hG4bK" 是 RFC3261 §7.2.3 规定的 branch 魔法前缀，平台用它识别事务，少了它
    //     有的网关直接丢包；
    //   - Max-Forwards: 70 是 RFC3261 建议默认值，本模块不做转发所以只是个占位。
    // From 的 tag 直接用 deviceCode：不随机也能工作，因为对端按 Call-ID + CSeq 认事务。
    QString message = QStringLiteral("REGISTER %1 SIP/2.0\r\nVia: SIP/2.0/UDP %2:%3;branch=%4;rport\r\nFrom: %5;tag=%6\r\nTo: %5\r\nCall-ID: %7\r\nCSeq: %8 REGISTER\r\nContact: <sip:%9@%2:%3>\r\nMax-Forwards: 70\r\nUser-Agent: %10\r\nExpires: %11\r\n")
        .arg(uri, advertisedAddress()).arg(config_.platform.advertiseSipPort).arg(branch).arg(from)
        .arg(runtime.config.deviceCode).arg(call).arg(runtime.cseq++).arg(runtime.config.deviceCode)
        .arg(config_.platform.userAgent).arg(config_.platform.registerExpires);
    if (withAuth) message += QStringLiteral("Authorization: %1\r\n").arg(digestAuthorization(runtime, QStringLiteral("REGISTER"), uri));
    message += QStringLiteral("Content-Length: 0\r\n\r\n");
    sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(static_cast<uint16_t>(config_.platform.serverPort));
    const QByteArray host = config_.platform.serverHost.toLocal8Bit();
    if (::inet_pton(AF_INET, host.constData(), &peer.sin_addr) != 1) return;
    sendMessage(message.toStdString(), peer);
    // 续注提前量 15 秒（sip/README.md 的口径）：注册有效期 Expires 由平台侧计时，
    // 卡着点续注会因为一次丢包就掉线。registerExpires 不大于 30（含配成 0 或负数）时
    // 一律按 15 秒一轮，避免算出 0/负间隔后每轮 select 都重发一次注册。
    runtime.nextRegisterMs = QDateTime::currentMSecsSinceEpoch() + (config_.platform.registerExpires > 30 ? (config_.platform.registerExpires - 15) * 1000LL : 15000);
}

/**
 * @brief 发一条 MESSAGE / Keepalive（<CmdType>Keepalive</CmdType><Status>OK</Status>）
 *
 * 前置：只有 registered 的流才会被 eventLoop 交进来发心跳——未注册不发，
 * 与平台侧"注册成功后才要求保活"的口径一致（heartbeatInterval 缺省 30 秒）。
 * 每次心跳新开一个 Call-ID（sendRegister 复用是刻意的区别：心跳是独立事务，
 * 复用会让平台把两次心跳当同一次重传）。
 *
 * 已知取舍：
 *   - <SN> 用的是**自增前**的 cseq、CSeq 头用自增后的值，两者同基不同值。国标只要求
 *     SN 单调递增，这样能工作；要按 SN 严格对账的平台需要单独给 SN 加计数器。
 *   - XML 声明写 GB2312、Content-Length 按 UTF-8 字节数算（见文件头），deviceCode 全 ASCII
 *     时等价；换成带中文的 Status 会直接长度不符。
 * 失败场景同 sendRegister：serverHost 非法则静默 return，且不推进 nextHeartbeatMs，
 * 于是下一轮 select（200ms 后）就会再试一次——填错地址时是"每 200ms 静默重试"而不是报错。
 */
void Gb28181Manager::sendHeartbeat(Runtime &runtime) {
    const QString domain = config_.platform.serverDomain.isEmpty() ? config_.platform.serverHost : config_.platform.serverDomain;
    const QString uri = QStringLiteral("sip:%1:%2").arg(config_.platform.serverHost).arg(config_.platform.serverPort);
    const QString from = QStringLiteral("<sip:%1@%2>").arg(runtime.config.deviceCode, domain);
    const QString targetCode = runtime.config.serverCode.isEmpty() ? config_.platform.serverCode : runtime.config.serverCode;
    const QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Notify><CmdType>Keepalive</CmdType><SN>%1</SN><DeviceID>%2</DeviceID><Status>OK</Status></Notify>")
        .arg(runtime.cseq).arg(xmlEscape(runtime.config.deviceCode));
    const QString call = newCallId(runtime.config.deviceCode);
    QString message = QStringLiteral("MESSAGE %1 SIP/2.0\r\nVia: SIP/2.0/UDP %2:%3;branch=z9hG4bK-%4;rport\r\nFrom: %5;tag=%6\r\nTo: <sip:%7@%8>\r\nCall-ID: %9\r\nCSeq: %10 MESSAGE\r\nContent-Type: Application/MANSCDP+xml\r\nUser-Agent: %11\r\nContent-Length: %12\r\n\r\n%13")
        .arg(uri, advertisedAddress()).arg(config_.platform.advertiseSipPort).arg(QDateTime::currentMSecsSinceEpoch()).arg(from)
        .arg(runtime.config.deviceCode).arg(targetCode).arg(domain).arg(call).arg(runtime.cseq++)
        .arg(config_.platform.userAgent).arg(xml.toUtf8().size()).arg(xml);
    sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(static_cast<uint16_t>(config_.platform.serverPort));
    const QByteArray host = config_.platform.serverHost.toLocal8Bit();
    if (::inet_pton(AF_INET, host.constData(), &peer.sin_addr) != 1) return;
    sendMessage(message.toStdString(), peer);
    runtime.nextHeartbeatMs = QDateTime::currentMSecsSinceEpoch() + config_.platform.heartbeatInterval * 1000LL;
}

/**
 * @brief 事件线程主体：先给每条流发一次裸 REGISTER，再进 select 循环
 *
 * streams_ / calls_ / config_ 只在这个线程里读写（start() 建完才起线程、stop() 先 join
 * 再清理），所以整段不加锁。running_ 是 atomic，由 stop() 置 false 来退出。
 *
 * 200ms 的 select 超时是信令响应性与 CPU 的折中：注册/心跳都按秒级窗口推进，
 * 更短只是空转，更长会让平台的重传窗口（RFC3261 T1=500ms）里收不到我们的响应。
 *
 * 收包缓冲 65536 字节：UDP 数据报上限就是 64KB（SIP over UDP 不允许分片外的体积），
 * 少留 1 字节给结尾 '\0'；超长报文会被 recvfrom 截断，截断后当普通报文解析、
 * 解不出就丢弃——实测国标 MANSCDP 请求都在 1.5KB 以内，不需要处理分片。
 */
void Gb28181Manager::eventLoop() {
    for (auto &runtime : streams_) sendRegister(*runtime, false);
    while (running_) {
        fd_set readSet; FD_ZERO(&readSet); if (socket_ >= 0) FD_SET(socket_, &readSet);
        timeval timeout{0, 200000};
        const int ready = socket_ >= 0 ? ::select(socket_ + 1, &readSet, nullptr, nullptr, &timeout) : 0;
        if (ready > 0 && FD_ISSET(socket_, &readSet)) {
            char buffer[65536]; sockaddr_in peer{}; socklen_t peerLength = sizeof(peer);
            const ssize_t length = ::recvfrom(socket_, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr *>(&peer), &peerLength);
            if (length > 0) { buffer[length] = '\0'; handleDatagram(std::string(buffer, static_cast<size_t>(length)), peer); }
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &runtime : streams_) {
            if (now >= runtime->nextRegisterMs) sendRegister(*runtime, !runtime->nonce.isEmpty());
            if (runtime->registered && now >= runtime->nextHeartbeatMs) sendHeartbeat(*runtime);
        }
    }
}

// 响应/请求的分派判据：RFC3261 规定状态行以 "SIP/2.0" 开头、请求行以方法名开头，
// 所以只看首 token 就够，不需要完整语法树。peer 原样带下去当回包目标（本机恒单 socket，
// 不做 Via 回溯）。
void Gb28181Manager::handleDatagram(const std::string &message, const sockaddr_in &peer) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    if (text.startsWith(QStringLiteral("SIP/2.0"))) handleResponse(message);
    else handleRequest(message, peer);
}

/**
 * @brief 处理平台回来的 SIP 响应（本模块只会收到注册相关的响应）
 *
 * 认领方式：按 Call-ID 比对 registerCallId，匹配不到**直接丢弃**——这是有意的：
 * 媒体会话（INVITE）的 200/错误响应也走同一个 socket，若不认领会把会话响应误当成
 * 注册结果改掉 registered。除注册外没有别的事务会收到响应（心跳/查询都只等 200，
 * 收到即落进这个"无人认领"分支）。
 *
 * 401 与 407 同等对待（Proxy-Authenticate / WWW-Authenticate 两个头分别取），
 * 因为有些平台前置了代理只回 407；只取挑战里 qop 列表的第一个值。
 * 非 2xx/401/407 的状态码（如 403 Forbidden、404）不置 registered 也不打日志，
 * 现象就是"一直注册不上"——NOTE: 这里缺一条 qWarning 便于现场定位，属已知不足。
 */
void Gb28181Manager::handleResponse(const std::string &message) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    Runtime *runtime = nullptr;
    const QString id = callId(text);
    for (auto &candidate : streams_) if (candidate->registerCallId == id) { runtime = candidate.get(); break; }
    if (!runtime) return;
    const int code = statusCode(text);
    if (code == 401 || code == 407) {
        const QString auth = headerValue(text, code == 401 ? QStringLiteral("WWW-Authenticate") : QStringLiteral("Proxy-Authenticate"));
        runtime->realm = digestField(auth, QStringLiteral("realm"));
        runtime->nonce = digestField(auth, QStringLiteral("nonce"));
        runtime->qop = digestField(auth, QStringLiteral("qop"));
        if (runtime->qop.contains(QLatin1Char(','))) runtime->qop = runtime->qop.section(QLatin1Char(','), 0, 0).trimmed();
        runtime->cnonce = md5(runtime->config.deviceCode + runtime->nonce).left(16);
        sendRegister(*runtime, true);
    } else if (code >= 200 && code < 300) {
        runtime->registered = true;
        runtime->nextHeartbeatMs = 0;
        qInfo() << "GB28181 registered" << runtime->config.deviceCode;
    }
}

/**
 * @brief 处理平台发来的请求：MESSAGE（查询）/ INVITE（点流）/ BYE（停流）
 *
 * 三级认领 Runtime 的顺序是刻意的，越可靠越靠前：
 *   1) calls_ 命中（INVITE 之后平台一定带同一个 Call-ID）；
 *   2) 报文里出现某条流的 channelCode/deviceCode 文本——粗暴但对 MANSCDP 有效，
 *      因为 <DeviceID> 就在 body 里，20 位编码基本不会误撞；
 *   3) 只有单条流且方法是 INVITE 时兜底认领该流（平台在目录里点的就是这唯一一路）。
 * 三条都不中则 runtime 为空：MESSAGE 仍会先回 200 OK 再丢弃查询，
 * 这样平台不会因为超时反复重发同一条 Query（回 200 与回 Response 是两件事）。
 *
 * 未处理的方法：ACK（推流在 200 OK 之后就已开始，不需要等 ACK）、CANCEL、订阅类请求，
 * 落到最后一条 else 静默忽略。BYE 找不到会话时只打一条 warning 不报错。
 */
void Gb28181Manager::handleRequest(const std::string &message, const sockaddr_in &peer) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    const QString method = requestMethod(text);
    Runtime *runtime = nullptr;
    const QString requestId = callId(text);
    const auto callIt = calls_.find(requestId.toStdString());
    if (callIt != calls_.end()) runtime = callIt->second;
    for (auto &candidate : streams_) {
        if (!runtime && (text.contains(candidate->config.channelCode) || text.contains(candidate->config.deviceCode))) {
            runtime = candidate.get();
            break;
        }
    }
    if (!runtime && method == QStringLiteral("INVITE") && streams_.size() == 1) runtime = streams_.front().get();
    if (method == QStringLiteral("MESSAGE")) {
        const QString via = headerValue(text, QStringLiteral("Via"));
        const QString from = headerValue(text, QStringLiteral("From"));
        const QString to = headerValue(text, QStringLiteral("To"));
        const QString id = callId(text);
        const QString cseq = headerValue(text, QStringLiteral("CSeq"));
        const QString response = QStringLiteral("SIP/2.0 200 OK\r\nVia: %1\r\nFrom: %2\r\nTo: %3\r\nCall-ID: %4\r\nCSeq: %5\r\nContent-Length: 0\r\n\r\n").arg(via, from, to, id, cseq);
        sendMessage(response.toStdString(), peer);
        if (runtime) handleQuery(*runtime, body(text), peer);
    } else if (method == QStringLiteral("INVITE") && runtime) handleInvite(*runtime, message, peer);
    else if (method == QStringLiteral("BYE") && runtime) handleBye(*runtime, message, peer);
    else if (method == QStringLiteral("BYE"))
        qWarning() << "GB28181 BYE did not match an active call" << requestId;
}

/**
 * @brief 应答 MANSCDP 查询（DeviceInfo / Catalog / DeviceStatus）
 * @param requestBody 已剥掉 SIP 头的 XML 正文；解析不出 CmdType 时按未知命令处理
 * @param peer        请求来源地址，Response 直接回这里（不做 Via 回溯）
 *
 * 只实现这三条：Broadcast / Subscribe / RecordInfo / Sd 等一律让 responseXml 保持空串，
 * 于是**一条 Response 都不发**（MESSAGE 的 200 OK 已在 handleRequest 里回过）。
 * 失效场景：平台等不到 Response 会按自己的超时重发 Query，表现为目录里"设备在线但查不出详情"，
 * 抓包能看到同一条 SN 的 Query 重复多次 —— 这时缺的是本函数支持的 CmdType，不是网络。
 *
 * Catalog 恒报 SumNum=1 / DeviceList Num="1"，Item 用 channelCode 而 ParentID 用 deviceCode：
 * 一条流 = 一个国标设备 = 一个通道，多通道聚合不在本轮范围内（README「不实现的部分」）。
 * Status 一律回 ON，不看媒体是否在推——平台按 INVITE 能否成功判可用性。
 */
void Gb28181Manager::handleQuery(Runtime &runtime, const QString &requestBody, const sockaddr_in &peer) {
    const QString command = xmlField(requestBody, QStringLiteral("CmdType"));
    const QString sn = xmlField(requestBody, QStringLiteral("SN"));
    const QString domain = config_.platform.serverDomain.isEmpty() ? config_.platform.serverHost : config_.platform.serverDomain;
    const QString serverCode = runtime.config.serverCode.isEmpty() ? config_.platform.serverCode : runtime.config.serverCode;
    QString responseXml;
    if (command == QStringLiteral("DeviceInfo")) {
        responseXml = QStringLiteral("<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response><CmdType>DeviceInfo</CmdType><SN>%1</SN><DeviceID>%2</DeviceID><DeviceName>%3</DeviceName><Manufacturer>idge</Manufacturer><Model>RK3588</Model><Firmware>1.0</Firmware><SerialNumber>%4</SerialNumber><Status>ON</Status></Response>")
            .arg(sn, xmlEscape(runtime.config.deviceCode), xmlEscape(runtime.config.deviceName), xmlEscape(runtime.config.serialNumber));
    } else if (command == QStringLiteral("Catalog")) {
        responseXml = QStringLiteral("<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response><CmdType>Catalog</CmdType><SN>%1</SN><DeviceID>%2</DeviceID><SumNum>1</SumNum><DeviceList Num=\"1\"><Item><DeviceID>%3</DeviceID><Name>%4</Name><Manufacturer>idge</Manufacturer><Model>RK3588</Model><Owner></Owner><CivilCode>%5</CivilCode><Address></Address><ParentID>%2</ParentID><Parental>1</Parental><SafetyWay>0</SafetyWay><RegisterWay>1</RegisterWay><Secrecy>0</Secrecy><Status>ON</Status></Item></DeviceList></Response>")
            .arg(sn, xmlEscape(runtime.config.deviceCode), xmlEscape(runtime.config.channelCode), xmlEscape(runtime.config.deviceName), xmlEscape(domain));
    } else if (command == QStringLiteral("DeviceStatus")) {
        responseXml = QStringLiteral("<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response><CmdType>DeviceStatus</CmdType><SN>%1</SN><DeviceID>%2</DeviceID><Result>OK</Result><Online>ONLINE</Online><Status>OK</Status><Encode>ON</Encode><Record>OFF</Record></Response>")
            .arg(sn, xmlEscape(runtime.config.deviceCode));
    }
    if (responseXml.isEmpty()) return;
    const QString uri = QStringLiteral("sip:%1@%2").arg(serverCode, domain);
    const QString from = QStringLiteral("<sip:%1@%2>").arg(runtime.config.deviceCode, domain);
    const QString call = newCallId(runtime.config.deviceCode);
    const QString message = QStringLiteral("MESSAGE %1 SIP/2.0\r\nVia: SIP/2.0/UDP %2:%3;branch=z9hG4bK-%4;rport\r\nFrom: %5;tag=%6\r\nTo: <sip:%7@%8>\r\nCall-ID: %9\r\nCSeq: %10 MESSAGE\r\nContent-Type: Application/MANSCDP+xml\r\nContent-Length: %11\r\n\r\n%12")
        .arg(uri, advertisedAddress()).arg(config_.platform.advertiseSipPort).arg(QDateTime::currentMSecsSinceEpoch()).arg(from)
        .arg(runtime.config.deviceCode).arg(serverCode).arg(domain).arg(call).arg(runtime.cseq++)
        .arg(responseXml.toUtf8().size()).arg(responseXml);
    sendMessage(message.toStdString(), peer);
}

/**
 * @brief 处理平台点流：协商收端与 SSRC → 回 200 OK（SDP answer）→ 起媒体线程
 *
 * 输入约束：SDP 必须有 `c=IN IP4 <地址>` 与 `m=video <端口>`，端口 0 视为无效；
 * 传输方式看 m= 行里有没有 "TCP"（`TCP/RTP/AVP` → tcp，其余 → udp），
 * **与配置里的 platform.transport / stream.transport 无关**（那两个键是假开关）。
 *
 * SSRC 协商（见 gb28181_ssrc.h）：平台给的 y= 合法就用它，否则用配置里的 ssrc，
 * 两个都不合法才回 488 Not Acceptable Here。配置也没填时现场生成的兜底值是
 * `100000 + (qHash(deviceCode) & 0x7FFFFF)`，即 100000~9437183：
 * 取 6 位以上是为了在 10 位补零的国标格式里不至于满前导零、方便人眼对日志；
 * 用 deviceCode 的哈希而不是随机数，是为了同一通道每次重启给出同一个 SSRC，
 * 平台侧抓包能对上前一次的流。
 *
 * 副作用与顺序（不可调换）：先 stopMedia() 再改 remoteHost/callId/calls_，
 * 因为媒体线程的回调正在读这些字段；最后才 startMedia()，此时 200 OK 已发出。
 * 失败场景：
 *   - 收端地址/端口不可用 → 直接 return，**连 480/486 都不回**，平台会重传 INVITE 到超时；
 *   - startMedia 里 RTP 出口打不开 → 200 OK 已经发出去了，平台以为在推流，
 *     现象是"点流成功但一直黑屏"，只有 qWarning 里的 "cannot open RTP destination" 能指认。
 */
void Gb28181Manager::handleInvite(Runtime &runtime, const std::string &message, const sockaddr_in &peer) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    const QString via = headerValue(text, QStringLiteral("Via"));
    const QString from = headerValue(text, QStringLiteral("From"));
    const QString to = headerValue(text, QStringLiteral("To"));
    const QString id = callId(text);
    const QString cseq = headerValue(text, QStringLiteral("CSeq"));
    const QString sdp = body(text);
    const QString connection = sdpField(sdp, QStringLiteral("c=IN IP4 "));
    const QString media = sdpField(sdp, QStringLiteral("m=video "));
    const QStringList mediaFields = media.split(QLatin1Char(' '), QString::SkipEmptyParts);
    const uint16_t port = mediaFields.isEmpty() ? 0 : static_cast<uint16_t>(mediaFields.at(0).toUShort());
    const QString transport = media.contains(QStringLiteral("TCP"), Qt::CaseInsensitive) ? QStringLiteral("tcp") : QStringLiteral("udp");
    qInfo() << "GB28181 INVITE" << runtime.config.deviceCode << "media=" << connection << port
            << "transport=" << transport;
    if (connection.isEmpty() || port == 0) {
        qWarning() << "GB28181 INVITE has no usable media destination" << runtime.config.deviceCode;
        return;
    }
    const QString offeredSsrc = sdpField(sdp, QStringLiteral("y="));
    const QString fallbackSsrc = runtime.config.ssrc.isEmpty()
        ? QString::number(100000 + static_cast<unsigned>(qHash(runtime.config.deviceCode) & 0x7FFFFF))
        : runtime.config.ssrc;
    std::string selectedSsrc;
    if (!gb28181SelectSsrc(offeredSsrc.toStdString(), fallbackSsrc.toStdString(), selectedSsrc)) {
        qWarning() << "GB28181 invalid SSRC" << runtime.config.deviceCode << "offered=" << offeredSsrc;
        const QString rejected = QStringLiteral("SIP/2.0 488 Not Acceptable Here\r\nVia: %1\r\nFrom: %2\r\nTo: %3;tag=%4\r\nCall-ID: %5\r\nCSeq: %6\r\nContent-Length: 0\r\n\r\n")
            .arg(via, from, to, runtime.config.deviceCode, id, cseq);
        sendMessage(rejected.toStdString(), peer);
        return;
    }
    // Join the old media worker before replacing any state read by its callbacks.
    stopMedia(runtime);
    calls_.erase(runtime.callId.toStdString());
    // 100 Trying 只能在 stopMedia() 之后发：上面那句 join 会阻塞到 ffmpeg 的
    // interrupt_callback 生效（开流卡住时最长几秒），期间平台收不到任何响应、
    // 可能重传 INVITE。已知取舍——宁可晚发 Trying，也不要先改状态再被旧线程读到半成品。
    const QString provisional = QStringLiteral("SIP/2.0 100 Trying\r\nVia: %1\r\nFrom: %2\r\nTo: %3\r\nCall-ID: %4\r\nCSeq: %5\r\nContent-Length: 0\r\n\r\n").arg(via, from, to, id, cseq);
    sendMessage(provisional.toStdString(), peer);
    runtime.callId = id;
    runtime.remoteHost = connection; runtime.remotePort = port; runtime.transport = transport;
    calls_[id.toStdString()] = &runtime;
    runtime.sessionSsrc = QString::fromStdString(selectedSsrc);
    const QString ssrc = runtime.sessionSsrc;
    qInfo() << "GB28181 SSRC negotiated" << runtime.config.deviceCode << "offered=" << offeredSsrc
            << "answer=" << ssrc << "rtpSsrc=" << ssrc.toUInt()
            << "hex=" << QStringLiteral("%1").arg(ssrc.toUInt(), 8, 16, QLatin1Char('0')).toUpper()
            << "tcpFraming=" << (transport == QStringLiteral("tcp") ? "RFC4571" : "UDP");
    // 应答 SDP 的 `m=video 9`：端口固定写 9（discard，RFC 保留端口）当占位。
    // 本机是纯发送方，不监听任何 RTP 收端，真实源端口由内核给 sendto 随机分配，
    // 平台按 y= 与 RTP 头里的 SSRC 认流——这也是 gb28181_rtp.h 里 open() 只需目的端的原因。
    // TCP 时 a=setup:active + a=connection:new 表示由本机主动 connect 平台（见 RtpSender::open）。
    const QString sdpAnswer = QStringLiteral("v=0\r\no=%1 0 0 IN IP4 %2\r\ns=Play\r\nc=IN IP4 %2\r\nt=0 0\r\nm=video 9 %3 96\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\na=setup:active\r\na=connection:new\r\ny=%4\r\n")
        .arg(runtime.config.deviceCode, advertisedAddress(), transport == QStringLiteral("tcp") ? QStringLiteral("TCP/RTP/AVP") : QStringLiteral("RTP/AVP"), ssrc);
    const QString answer = QStringLiteral("SIP/2.0 200 OK\r\nVia: %1\r\nFrom: %2\r\nTo: %3;tag=%4\r\nCall-ID: %5\r\nCSeq: %6\r\nContact: <sip:%7@%8:%9>\r\nContent-Type: application/sdp\r\nContent-Length: %10\r\n\r\n%11")
        .arg(via, from, to, runtime.config.deviceCode, id, cseq, runtime.config.deviceCode, advertisedAddress())
        .arg(config_.platform.advertiseSipPort).arg(sdpAnswer.toUtf8().size()).arg(sdpAnswer);
    sendMessage(answer.toStdString(), peer);
    startMedia(runtime);
}

/**
 * @brief 平台挂断：回 200 OK 并停掉这路的媒体与 RTP
 *
 * 只停媒体、不删配置也不 reset 认证参数，所以平台可以立刻再 INVITE 同一路重新起流。
 * 副作用：calls_ 里这一项被摘掉、runtime.callId 清空；registered / nextHeartbeatMs 不动，
 * 注册事务与媒体会话本来就是两条独立的线。
 * 前置：由 handleRequest 保证 runtime 是 calls_ 里的那一路，这里不再校验 callId 是否等于
 * 请求里的 Call-ID（NOTE: 平台用别的 Call-ID 发 BYE 时会停错当前会话，实测平台不会这样发）。
 */
void Gb28181Manager::handleBye(Runtime &runtime, const std::string &message, const sockaddr_in &peer) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    qInfo() << "GB28181 BYE" << runtime.config.deviceCode << "call=" << callId(text);
    const QString response = QStringLiteral("SIP/2.0 200 OK\r\nVia: %1\r\nFrom: %2\r\nTo: %3\r\nCall-ID: %4\r\nCSeq: %5\r\nContent-Length: 0\r\n\r\n")
        .arg(headerValue(text, QStringLiteral("Via")), headerValue(text, QStringLiteral("From")), headerValue(text, QStringLiteral("To")),
             callId(text), headerValue(text, QStringLiteral("CSeq")));
    sendMessage(response.toStdString(), peer);
    stopMedia(runtime);
    calls_.erase(runtime.callId.toStdString());
    runtime.callId.clear();
}

/**
 * @brief 按 INVITE 协商出来的收端开 RTP 出口，再起这路的媒体线程
 *
 * 开头先 stopMedia()：INVITE 可能在没有 BYE 的情况下重发（平台超时重传 / 用户反复点流），
 * 一路同时存在两条媒体线程会互相灌包，PS 包头尾交错、平台解不出画面。
 * 三个计数器/缓存必须在 media->start() 之前复位：muxer->reset() 少了这一步，
 * 新会话会沿用上一个会话缓存的 SPS/PPS 直接起发（见 gb28181_rtp.h 的 reset 说明）。
 *
 * 失败场景：rtp->open() 失败只 qWarning 后 return，**此时 200 OK 已经发出**，
 * 平台侧显示"点流成功"但没有码流；调用方没有回滚会话，等下一次 INVITE 或 BYE 才恢复。
 * 副作用：pushing 置 true；media 的帧回调在媒体线程上直接调 muxer，不经过本事件循环。
 */
void Gb28181Manager::startMedia(Runtime &runtime) {
    stopMedia(runtime);
    // 取流地址不在这段配置里，日志要把"引用的哪条流"和"解析出来实际开哪个地址"都打出来：
    // 现场排查平台点流黑屏时，这两行就能分清是 source_id 指错了还是注册表里的 url 本身不对。
    const VideoSourceCfg take = runtime.config.takeSource();
    qInfo() << "GB28181 starting media" << runtime.config.deviceCode
            << "source_id=" << runtime.config.sourceId
            << "url=" << (take.url.isEmpty() ? QStringLiteral("<注册表里查不到>") : take.url)
            << "type=" << take.type
            << "rtp=" << runtime.remoteHost << runtime.remotePort << runtime.transport;
    if (!runtime.rtp->open(runtime.remoteHost.toStdString(), runtime.remotePort, runtime.transport.toStdString(), runtime.sessionSsrc.toStdString())) {
        qWarning() << "GB28181 cannot open RTP destination" << runtime.remoteHost << runtime.remotePort;
        return;
    }
    qInfo() << "GB28181 RTP destination opened" << runtime.config.deviceCode;
    runtime.muxer->reset();
    runtime.psFrames = 0;
    runtime.rtpSendErrors = 0;
    Runtime *runtimePtr = &runtime;
    runtime.media->start([runtimePtr](const Gb28181EncodedFrame &frame) {
        if (runtimePtr->muxer) {
            runtimePtr->muxer->writeVideoFrame(frame.data, static_cast<uint32_t>(frame.pts90k), frame.keyFrame);
        }
    }, [runtimePtr](const QString &error) { qWarning() << "GB28181 source" << runtimePtr->config.deviceCode << error; });
    runtime.pushing = true;
    qInfo() << "GB28181 media worker started" << runtime.config.deviceCode;
}

/**
 * @brief 停这路的媒体线程并关 RTP 出口；幂等，没起过也能调
 *
 * media->stop() 会 join 工作线程，最长阻塞到 ffmpeg 的 interrupt_callback 生效，
 * 因此只能在事件线程（handleInvite/handleBye）或 stop() 里调，不要在响应回调里调。
 * 判重条件写作 pushing || media->running()：前者是本机记的"起过没停"，后者覆盖
 * "媒体线程自己开流失败退出、pushing 还留着 true"这种情况，日志才不会漏一条统计。
 * psFrames / rtpSendErrors 是普通 uint64（非原子），这里读它们只用于打日志。
 */
void Gb28181Manager::stopMedia(Runtime &runtime) {
    if (runtime.pushing || (runtime.media && runtime.media->running()))
        qInfo() << "GB28181 stopping media" << runtime.config.deviceCode
                << "psFrames=" << runtime.psFrames << "rtpErrors=" << runtime.rtpSendErrors;
    if (runtime.media) runtime.media->stop();
    if (runtime.rtp) runtime.rtp->stop();
    runtime.pushing = false;
    if (runtime.media && !runtime.media->running())
        qInfo() << "GB28181 media stopped" << runtime.config.deviceCode;
}
