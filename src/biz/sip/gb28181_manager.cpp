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

int statusCode(const QString &message) {
    const QString first = message.section(QStringLiteral("\r\n"), 0, 0);
    const QStringList fields = first.split(QLatin1Char(' '), QString::SkipEmptyParts);
    return fields.size() > 1 ? fields.at(1).toInt() : 0;
}

QString requestMethod(const QString &message) {
    const QString first = message.section(QStringLiteral("\r\n"), 0, 0);
    return first.section(QLatin1Char(' '), 0, 0).trimmed();
}

QString body(const QString &message) {
    const int separator = message.indexOf(QStringLiteral("\r\n\r\n"));
    return separator < 0 ? QString() : message.mid(separator + 4);
}

QString digestField(const QString &header, const QString &field) {
    const QRegularExpression expression(QStringLiteral("%1\\s*=\\s*\\\"([^\\\"]+)\\\"").arg(field));
    const auto match = expression.match(header);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString md5(const QString &value) {
    return QString::fromLatin1(QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Md5).toHex());
}

QString xmlEscape(QString value) {
    return value.replace(QLatin1Char('&'), QStringLiteral("&amp;")).replace(QLatin1Char('<'), QStringLiteral("&lt;"))
        .replace(QLatin1Char('>'), QStringLiteral("&gt;"));
}

QString sdpField(const QString &sdp, const QString &prefix) {
    const QStringList lines = sdp.split(QLatin1Char('\n'));
    for (const QString &line : lines) if (line.startsWith(prefix)) return line.mid(prefix.size()).trimmed();
    return {};
}

QString xmlField(const QString &xml, const QString &name) {
    const QRegularExpression expression(QStringLiteral("<%1>([^<]*)</%1>").arg(name));
    const auto match = expression.match(xml);
    return match.hasMatch() ? match.captured(1) : QString();
}
}

Gb28181Manager::Gb28181Manager() = default;

Gb28181Manager::~Gb28181Manager() { stop(); }

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

QString Gb28181Manager::advertisedAddress() const {
    return config_.platform.advertiseHost;
}

QString Gb28181Manager::newCallId(const QString &deviceCode) {
    return QStringLiteral("%1-%2-%3@%4").arg(deviceCode).arg(QDateTime::currentMSecsSinceEpoch()).arg(callCounter_++).arg(advertisedAddress());
}

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

void Gb28181Manager::sendMessage(const std::string &message, const sockaddr_in &peer) {
    if (socket_ >= 0) ::sendto(socket_, message.data(), message.size(), MSG_NOSIGNAL,
                                reinterpret_cast<const sockaddr *>(&peer), sizeof(peer));
}

void Gb28181Manager::sendRegister(Runtime &runtime, bool withAuth) {
    const QString domain = config_.platform.serverDomain.isEmpty() ? config_.platform.serverHost : config_.platform.serverDomain;
    const QString uri = QStringLiteral("sip:%1:%2").arg(config_.platform.serverHost).arg(config_.platform.serverPort);
    const QString from = QStringLiteral("<sip:%1@%2>").arg(runtime.config.deviceCode, domain);
    const QString call = runtime.registerCallId.isEmpty() ? newCallId(runtime.config.deviceCode) : runtime.registerCallId;
    runtime.registerCallId = call;
    const QString branch = QStringLiteral("z9hG4bK-%1").arg(QDateTime::currentMSecsSinceEpoch());
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
    runtime.nextRegisterMs = QDateTime::currentMSecsSinceEpoch() + (config_.platform.registerExpires > 30 ? (config_.platform.registerExpires - 15) * 1000LL : 15000);
}

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

void Gb28181Manager::handleDatagram(const std::string &message, const sockaddr_in &peer) {
    const QString text = QString::fromUtf8(message.data(), static_cast<int>(message.size()));
    if (text.startsWith(QStringLiteral("SIP/2.0"))) handleResponse(message);
    else handleRequest(message, peer);
}

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
    const QString sdpAnswer = QStringLiteral("v=0\r\no=%1 0 0 IN IP4 %2\r\ns=Play\r\nc=IN IP4 %2\r\nt=0 0\r\nm=video 9 %3 96\r\na=sendonly\r\na=rtpmap:96 PS/90000\r\na=setup:active\r\na=connection:new\r\ny=%4\r\n")
        .arg(runtime.config.deviceCode, advertisedAddress(), transport == QStringLiteral("tcp") ? QStringLiteral("TCP/RTP/AVP") : QStringLiteral("RTP/AVP"), ssrc);
    const QString answer = QStringLiteral("SIP/2.0 200 OK\r\nVia: %1\r\nFrom: %2\r\nTo: %3;tag=%4\r\nCall-ID: %5\r\nCSeq: %6\r\nContact: <sip:%7@%8:%9>\r\nContent-Type: application/sdp\r\nContent-Length: %10\r\n\r\n%11")
        .arg(via, from, to, runtime.config.deviceCode, id, cseq, runtime.config.deviceCode, advertisedAddress())
        .arg(config_.platform.advertiseSipPort).arg(sdpAnswer.toUtf8().size()).arg(sdpAnswer);
    sendMessage(answer.toStdString(), peer);
    startMedia(runtime);
}

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
