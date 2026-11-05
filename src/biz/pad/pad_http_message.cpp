// ============================================================================
// pad_http_message.cpp — v1 HTTP 面的报文解析与信封拼装
// ============================================================================
//
// 实现口径逐条对着 plan/edge-pad-v1.md §2.4/§2.6/§13.3.2 写；
// 本文件里**没有**任何协议业务规则（哪些字段必填、哪种状态算终态都在各路由里），
// 只有"字节 ↔ Request/Reply"和"信封长什么样"两件事。
// 为什么这么切：这样 §4~§7 的路由能拿同一个 Request 测，而不必真起一条 socket。
// ============================================================================
#include "pad_http_message.h"

#include <QByteArray>
#include <QJsonParseError>
#include <QStringList>
#include <QUrl>

namespace padhttp {

namespace {

const char kCrlf[] = "\r\n";
const QByteArray kCrlfBytes = QByteArray("\r\n", 2);
const QByteArray kCrlfCrlf = QByteArray("\r\n\r\n", 4);

// 十六进制字符 → 数值；非十六进制返回 -1
int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Content-Disposition 里 key="value" 的取值（不带引号也能取，虽然 RFC 6266 要求带）。
// 找不到 key 返回空串。
QString dispositionParam(const QString &line, const QString &key)
{
    const int start = line.indexOf(key + QLatin1Char('='));
    if (start < 0)
        return QString();
    int i = start + key.size() + 1;
    if (i >= line.size())
        return QString();
    if (line.at(i) == QLatin1Char('"')) {
        ++i;
        const int end = line.indexOf(QLatin1Char('"'), i);
        if (end < 0)
            return QString();
        return line.mid(i, end - i);
    }
    int end = i;
    while (end < line.size() && line.at(end) != QLatin1Char(';'))
        ++end;
    return line.mid(i, end - i).trimmed();
}

}  // namespace

// ============================================================================
// Request
// ============================================================================

QString Request::header(const QString &name) const
{
    return headers.value(name.toLower());
}

QString Request::queryValue(const QString &key) const
{
    return query.value(key);
}

QString Request::pathParam(const QString &key) const
{
    return pathParams.value(key);
}

bool Request::isJsonContent() const
{
    return contentType.startsWith(QLatin1String("application/json"));
}

bool Request::parseJsonObject(const QByteArray &raw, QJsonObject *out, QString *err)
{
    if (out)
        *out = QJsonObject();
    if (raw.trimmed().isEmpty()) {
        if (err)
            *err = QString::fromUtf8("请求体是空的");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (doc.isNull()) {
        if (err) {
            *err = QString::fromUtf8("请求体不是合法 JSON：%1").arg(parseError.errorString());
        }
        return false;
    }
    if (!doc.isObject()) {
        if (err)
            *err = QString::fromUtf8("请求体顶层必须是 JSON 对象");
        return false;
    }
    if (out)
        *out = doc.object();
    return true;
}

bool Request::jsonBody(QJsonObject *out, QString *err) const
{
    return parseJsonObject(body, out, err);
}

// ============================================================================
// 状态行 / 响应序列化
// ============================================================================

QString reasonPhrase(int status)
{
    switch (status) {
    case 200: return QStringLiteral("OK");
    case 201: return QStringLiteral("Created");
    case 204: return QStringLiteral("No Content");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 409: return QStringLiteral("Conflict");
    case 422: return QStringLiteral("Unprocessable Entity");
    case 500: return QStringLiteral("Internal Server Error");
    case 503: return QStringLiteral("Service Unavailable");
    default:  return QStringLiteral("Status");
    }
}

QByteArray serializeReply(const Reply &reply, bool keepAlive)
{
    QByteArray out;
    out.reserve(reply.body.size() + 256);
    out += "HTTP/1.1 ";
    out += QByteArray::number(reply.status);
    out += ' ';
    out += reasonPhrase(reply.status).toUtf8();
    out += kCrlfBytes;

    // 追加头先写，并且**去重**：extraHeaders 里已经给了 Content-Type / Connection
    // 就不再写默认那两个——出现两个同名头时客户端只用最后一个，那是没人能预测的行为。
    bool hasContentType = false;
    bool hasConnection = false;
    for (const auto &kv : reply.extraHeaders) {
        const QString lower = kv.first.toLower();
        if (lower == QLatin1String("content-type"))
            hasContentType = true;
        if (lower == QLatin1String("connection"))
            hasConnection = true;
        out += kv.first.toUtf8();
        out += ": ";
        out += kv.second.toUtf8();
        out += kCrlfBytes;
    }
    if (!hasContentType) {
        out += "Content-Type: ";
        out += reply.contentType.toUtf8();
        out += kCrlfBytes;
    }
    out += "Content-Length: ";
    out += QByteArray::number(reply.body.size());
    out += kCrlfBytes;
    if (!hasConnection) {
        out += keepAlive ? "Connection: keep-alive" : "Connection: close";
        out += kCrlfBytes;
    }
    out += kCrlfBytes;
    out += reply.body;
    return out;
}

// ============================================================================
// §2.4 信封
// ============================================================================

QByteArray envelopeJson(int code, const QString &message, const QString &detail,
                        const QJsonValue &data)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("code"), code);
    obj.insert(QStringLiteral("message"), message);
    // detail 只进日志；空串写成 null，免得对端把空字符串当成一句可展示的话
    obj.insert(QStringLiteral("detail"),
               detail.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(detail));
    obj.insert(QStringLiteral("data"), data);
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

Reply replyOk(const QJsonValue &data)
{
    Reply reply;
    reply.status = 200;
    reply.body = envelopeJson(0, QStringLiteral("ok"), QString(), data);
    return reply;
}

Reply replyError(int httpStatus, int code, const QString &message, const QString &detail)
{
    Reply reply;
    reply.status = httpStatus;
    reply.body = envelopeJson(code, message, detail, QJsonValue(QJsonValue::Null));
    return reply;
}

Reply replyBadRequest(const QString &message, const QString &detail)
{
    return replyError(400, 2001, message, detail);
}

Reply replyUnauthorized(const QString &message, const QString &detail)
{
    return replyError(401, 1002, message, detail);
}

Reply replyNotFound(const QString &message, const QString &detail)
{
    return replyError(404, 2002, message, detail);
}

Reply replyConflict(const QString &message, const QString &detail)
{
    return replyError(409, 2003, message, detail);
}

Reply replyConflictCode(int code, const QString &message, const QString &detail)
{
    return replyError(409, code, message, detail);
}

Reply replyValidation(const QString &message, const QString &detail)
{
    return replyError(422, 4001, message, detail);
}

Reply replyInternal(const QString &message, const QString &detail)
{
    return replyError(500, 3001, message, detail);
}

Reply replyUnavailable(const QString &message, const QString &detail)
{
    return replyError(503, 3002, message, detail);
}

// ============================================================================
// 解析工具
// ============================================================================

QString percentDecode(const QString &encoded)
{
    // 手写而不是 QUrl::fromPercentEncoding：那个会把非法转义段整体丢掉，
    // 而这里要的是"非法就原样留着"，让畸形 ID 走到业务层被判"不存在"，
    // 而不是在传输层变成一句"参数错误"。
    //
    // 关键：转义解出的字节与没转义的字节要**攒进同一个 QByteArray，最后整体 fromUtf8**。
    // 逐字节转 QChar 会把 "%E4%B8%AD" 这种一个汉字拆成三个 ISO-8859-1 字符
    // （板端自测实测：解码结果里找不到"中国"）。
    QByteArray decoded;
    decoded.reserve(encoded.size());
    const QByteArray bytes = encoded.toUtf8();
    int i = 0;
    while (i < bytes.size()) {
        const char c = bytes.at(i);
        if (c == '%' && i + 2 < bytes.size()) {
            const int hi = hexValue(bytes.at(i + 1));
            const int lo = hexValue(bytes.at(i + 2));
            if (hi >= 0 && lo >= 0) {
                decoded.append(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        if (c == '%') {
            decoded.append('%');   // 截断的 %xx：保留这个 %，后面的字节照常处理
            ++i;
            continue;
        }
        const int next = bytes.indexOf('%', i);
        // 从 i 吃到下一个 %（或结尾）——这一段本来就是 UTF-8，直接搬
        const int take = next < 0 ? bytes.size() - i : next - i;
        decoded.append(bytes.constData() + i, take);
        i += take;
    }
    return QString::fromUtf8(decoded);
}

QMap<QString, QString> parseQuery(const QString &rawQuery)
{
    QMap<QString, QString> out;
    const QStringList pairs = rawQuery.split(QLatin1Char('&'), QString::SkipEmptyParts);
    for (const QString &pair : pairs) {
        const int eq = pair.indexOf(QLatin1Char('='));
        const QString key = percentDecode(eq < 0 ? pair : pair.left(eq));
        if (key.isEmpty())
            continue;
        const QString value = eq < 0 ? QString() : percentDecode(pair.mid(eq + 1));
        // 同名键保留第一个：QMap::insert 会覆盖，所以先判存在。
        // 为什么不是"保留最后一个"或"收成数组"：v1 的查询串里每个键只有一个值，
        // 出现两次多半是中间层拼错了，取第一个是最早的那个意图，也最容易被日志复现。
        if (!out.contains(key))
            out.insert(key, value);
    }
    return out;
}

QString multipartBoundary(const QString &contentType)
{
    const int at = contentType.indexOf(QStringLiteral("boundary="), 0, Qt::CaseInsensitive);
    if (at < 0)
        return QString();
    QString value = contentType.mid(at + QStringLiteral("boundary=").size());
    const int semi = value.indexOf(QLatin1Char(';'));
    if (semi >= 0)
        value = value.left(semi);
    value = value.trimmed();
    if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) && value.size() >= 2)
        value = value.mid(1, value.size() - 2);
    return value;
}

bool parseMultipart(const QByteArray &body, const QString &boundary,
                    QList<Part> *out, QString *err, int maxParts)
{
    if (out)
        out->clear();
    if (boundary.isEmpty()) {
        if (err)
            *err = QString::fromUtf8("Content-Type 里没有 multipart 的 boundary");
        return false;
    }
    if (!out) {
        if (err)
            *err = QString::fromUtf8("内部错误：解析 multipart 时没给出参");
        return false;
    }
    const QByteArray dash = "--" + boundary.toUtf8();
    int pos = body.indexOf(dash);
    if (pos < 0) {
        if (err)
            *err = QString::fromUtf8("正文里找不到 multipart 定界串");
        return false;
    }
    pos += dash.size();

    for (;;) {
        // 定界串之后要么紧跟 "--"（结束），要么跟 CRLF 再接 part 头
        if (pos + 2 <= body.size() && body.mid(pos, 2) == "--")
            return true;
        if (pos + 2 > body.size() || body.mid(pos, 2) != kCrlfBytes) {
            if (err)
                *err = QString::fromUtf8("multipart 定界行后面不是 CRLF");
            return false;
        }
        pos += 2;
        const int next = body.indexOf(dash, pos);
        if (next < 0) {
            if (err)
                *err = QString::fromUtf8("multipart 缺少结束定界");
            return false;
        }
        if (out->size() >= maxParts) {
            if (err)
                *err = QString::fromUtf8("multipart 的 part 数量超过上限 %1").arg(maxParts);
            return false;
        }
        QByteArray segment = body.mid(pos, next - pos);
        // 定界串前面按规定有一个 CRLF，它不属于正文
        if (segment.endsWith(kCrlfBytes))
            segment.chop(2);
        const int sep = segment.indexOf(kCrlfCrlf);
        if (sep < 0) {
            if (err)
                *err = QString::fromUtf8("multipart 的某个 part 缺少头部结束空行");
            return false;
        }
        Part part;
        const QList<QByteArray> headerLines =
            segment.left(sep).split('\n');
        for (const QByteArray &rawLine : headerLines) {
            const QByteArray line = rawLine.trimmed();
            if (line.isEmpty())
                continue;
            const int colon = line.indexOf(':');
            if (colon < 0)
                continue;
            const QByteArray key = line.left(colon).trimmed().toLower();
            const QString value = QString::fromUtf8(line.mid(colon + 1).trimmed());
            if (key == "content-disposition") {
                part.name = dispositionParam(value, QStringLiteral("name"));
                part.filename = dispositionParam(value, QStringLiteral("filename"));
            } else if (key == "content-type") {
                part.contentType = value;
            }
        }
        part.data = segment.mid(sep + 4);
        out->append(part);
        pos = next + dash.size();
    }
}

// ============================================================================
// RequestDecoder
// ============================================================================

void RequestDecoder::setLimits(int maxHeadBytes, qint64 maxBodyBytes)
{
    // 下限夹住：传 0 进去等于"一个字节都不许收"，表现是每条连接一上来就 400，
    // 排查起来完全看不出是被配置顶掉的，所以这里给一个不可能误用的地板值。
    maxHeadBytes_ = qBound(256, maxHeadBytes, 1 << 20);
    maxBodyBytes_ = maxBodyBytes > 0 ? maxBodyBytes : (26LL * 1024 * 1024);
}

bool RequestDecoder::feed(const QByteArray &incoming)
{
    if (state_ == State::Failed)
        return false;
    if (!incoming.isEmpty())
        buf_.append(incoming);
    return advance();
}

bool RequestDecoder::advance()
{
    if (state_ == State::ReadingHead && !parseHead())
        return false;
    if (state_ == State::ReadingBody) {
        if (chunked_ ? !consumeChunkedBody() : !consumeFixedBody())
            return false;
    }
    return state_ != State::Failed;
}

bool RequestDecoder::parseHead()
{
    const int sep = buf_.indexOf(kCrlfCrlf);
    // 上限对"已经收完的头"和"还没收完的头"都要判。只在后一种判等于没判：
    // 一个 2000 字节头的报文往往正好一次 readyRead 就到（板端自测实测：
    // maxHead=1024 时这种报文照样被解成合法请求）。含未收完的最后一行时边界略松，
    // 换掉"永远收不完"这个失效方向才是本意。
    const int headBytes = sep < 0 ? buf_.size() : sep + 4;
    if (headBytes > maxHeadBytes_) {
        fail(QString::fromUtf8("请求行与请求头合计超过 %1 字节").arg(maxHeadBytes_));
        return false;
    }
    if (sep < 0)
        return true;
    const QByteArray headBlock = buf_.left(sep);
    buf_.remove(0, sep + 4);

    const int lineEnd = headBlock.indexOf(kCrlfBytes);
    const QByteArray requestLine = lineEnd < 0 ? headBlock : headBlock.left(lineEnd);
    const int first = requestLine.indexOf(' ');
    const int second = first < 0 ? -1 : requestLine.indexOf(' ', first + 1);
    if (first < 0 || second < 0) {
        fail(QString::fromUtf8("请求行不是「方法 目标 版本」三段"));
        return false;
    }
    method_ = QString::fromLatin1(requestLine.left(first)).toUpper();
    version_ = QString::fromLatin1(requestLine.mid(second + 1).trimmed());
    target_ = QString::fromUtf8(requestLine.mid(first + 1, second - first - 1));

    // 头：逐行切，键转小写，同名只留第一个（与 parseQuery 同一口径）
    headers_.clear();
    contentType_.clear();
    boundary_.clear();
    chunked_ = false;
    expectBytes_ = -1;
    const QList<QByteArray> lines = headBlock.mid(lineEnd + 2).split('\n');
    for (const QByteArray &rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = QString::fromLatin1(line.left(colon).trimmed()).toLower();
        const QString value = QString::fromUtf8(line.mid(colon + 1).trimmed());
        if (headers_.contains(key))
            continue;
        headers_.insert(key, value);
        if (key == QLatin1String("content-length")) {
            bool ok = false;
            const qint64 len = value.toLongLong(&ok);
            if (!ok || len < 0) {
                fail(QString::fromUtf8("Content-Length 不是非负整数：%1").arg(value));
                return false;
            }
            expectBytes_ = len;
        } else if (key == QLatin1String("transfer-encoding")) {
            if (value.contains(QStringLiteral("chunked"), Qt::CaseInsensitive))
                chunked_ = true;
        } else if (key == QLatin1String("content-type")) {
            contentType_ = value;
        }
    }
    boundary_ = multipartBoundary(contentType_);

    const int question = target_.indexOf(QLatin1Char('?'));
    path_ = percentDecode(question < 0 ? target_ : target_.left(question));
    query_ = parseQuery(question < 0 ? QString() : target_.mid(question + 1));

    // 正文有没有、收多少：chunked 优先（同时给两个头时 HTTP 规定以 chunked 为准）。
    // 都不给就是没有正文——这里**不看方法**判，POST 带 Content-Length:0 与 GET 无正文
    // 走的是同一条"直接完成"分支。
    if (chunked_) {
        state_ = State::ReadingBody;
        chunkPhase_ = 0;
        chunkRemaining_ = 0;
        return true;
    }
    if (expectBytes_ > maxBodyBytes_) {
        // 提前拒绝，不先收 26MB 再嫌大：那种写法让一句请求就能把内存吃掉上限。
        fail(QString::fromUtf8("正文声明的 %1 字节超过上限 %2 字节")
                 .arg(expectBytes_)
                 .arg(maxBodyBytes_));
        return false;
    }
    if (expectBytes_ > 0) {
        state_ = State::ReadingBody;
        return true;
    }
    finishRequest();
    return true;
}

bool RequestDecoder::consumeFixedBody()
{
    const qint64 need = expectBytes_ - body_.size();
    if (need <= 0) {
        finishRequest();
        return true;
    }
    if (buf_.isEmpty())
        return true;   // 等下一次 readyRead
    const int take = static_cast<int>(qMin(need, static_cast<qint64>(buf_.size())));
    body_.append(buf_.left(take));
    buf_.remove(0, take);
    if (static_cast<qint64>(body_.size()) > maxBodyBytes_) {
        fail(QString::fromUtf8("正文实际字节数超过上限 %1").arg(maxBodyBytes_));
        return false;
    }
    if (static_cast<qint64>(body_.size()) >= expectBytes_)
        finishRequest();
    return true;
}

bool RequestDecoder::consumeChunkedBody()
{
    for (;;) {
        switch (chunkPhase_) {
        case 0: {   // 等一行「十六进制长度[;扩展]」
            const int idx = buf_.indexOf(kCrlfBytes);
            if (idx < 0) {
                if (buf_.size() > maxHeadBytes_) {
                    fail(QString::fromUtf8("chunk 长度行超过 %1 字节").arg(maxHeadBytes_));
                    return false;
                }
                return true;   // 等更多
            }
            QByteArray line = buf_.left(idx);
            buf_.remove(0, idx + 2);
            const int semi = line.indexOf(';');
            if (semi >= 0)
                line = line.left(semi);
            bool ok = false;
            const qint64 size = line.trimmed().toLongLong(&ok, 16);
            if (!ok || size < 0) {
                fail(QString::fromUtf8("chunk 长度行不是十六进制：%1")
                         .arg(QString::fromUtf8(line)));
                return false;
            }
            if (size == 0) {
                chunkPhase_ = 3;   // 跳到 trailer
                continue;
            }
            chunkRemaining_ = size;
            chunkPhase_ = 1;
            continue;
        }
        case 1: {   // 抄 size 个字节
            if (chunkRemaining_ <= 0) {
                chunkPhase_ = 2;
                continue;
            }
            if (buf_.isEmpty())
                return true;
            const int take = static_cast<int>(qMin(chunkRemaining_, static_cast<qint64>(buf_.size())));
            body_.append(buf_.left(take));
            buf_.remove(0, take);
            chunkRemaining_ -= take;
            if (static_cast<qint64>(body_.size()) > maxBodyBytes_) {
                fail(QString::fromUtf8("正文实际字节数超过上限 %1").arg(maxBodyBytes_));
                return false;
            }
            if (chunkRemaining_ == 0)
                chunkPhase_ = 2;
            continue;
        }
        case 2: {   // 每段正文后面必须跟一个 CRLF
            if (buf_.size() < 2)
                return true;
            if (buf_.left(2) != kCrlfBytes) {
                fail(QString::fromUtf8("chunk 正文结尾不是 CRLF"));
                return false;
            }
            buf_.remove(0, 2);
            chunkPhase_ = 0;
            continue;
        }
        default: {   // 3：最后一个 chunk 之后的 trailer 头，到空行为止
            const int idx = buf_.indexOf(kCrlfBytes);
            if (idx < 0) {
                if (buf_.size() > maxHeadBytes_) {
                    fail(QString::fromUtf8("chunk 尾部头超过 %1 字节").arg(maxHeadBytes_));
                    return false;
                }
                return true;
            }
            if (idx == 0) {
                buf_.remove(0, 2);
                finishRequest();
                return true;
            }
            buf_.remove(0, idx + 2);   // 丢掉一行 trailer（本层不用它的值）
            continue;
        }
        }
    }
}

void RequestDecoder::finishRequest()
{
    state_ = State::Complete;
}

void RequestDecoder::fail(const QString &text)
{
    error_ = text;
    state_ = State::Failed;
}

bool RequestDecoder::takeRequest(Request *out)
{
    if (state_ != State::Complete)
        return false;
    if (!out)
        return false;

    Request req;
    req.method = method_;
    req.version = version_;
    req.target = target_;
    req.path = path_;
    req.query = query_;
    req.headers = headers_;
    req.body = body_;
    req.contentType = contentType_;
    req.boundary = boundary_;

    if (!req.boundary.isEmpty()) {
        QString parseError;
        if (!parseMultipart(req.body, req.boundary, &req.parts, &parseError)) {
            // 正文都收完了才发现形状不对：照样判失败（400/2001），
            // 并把解码器置成 Failed——调用方读完 errorText() 就知道要关连接，
            // 不能在这条连接上继续复用（剩下的字节归属已经不可信）。
            fail(parseError);
            return false;
        }
    }

    // 复位：body/头清掉，但 buf_ 里没消费的字节留给同一连接的下一个请求
    method_.clear();
    version_.clear();
    target_.clear();
    path_.clear();
    query_.clear();
    headers_.clear();
    contentType_.clear();
    boundary_.clear();
    body_.clear();
    chunked_ = false;
    expectBytes_ = -1;
    chunkPhase_ = 0;
    chunkRemaining_ = 0;
    error_.clear();
    state_ = State::ReadingHead;

    *out = req;
    // 取走一个之后立刻把**缓冲里剩下的字节**接着解：客户端在 keep-alive 上不等回复
    // 就连发第二个请求时（pipelining），第二个请求可能早就到了。不在这里推进，
    // 调用方就会以为"还在等字节"，而那个 readyRead 可能永远不来（空闲超时才关连接）。
    // 返回值忽略：本函数已经交出一个合法请求；推进失败只把 state 置成 Failed，
    // 由调用方在取干之后看 isFailed() 决定关连接。
    advance();
    return true;
}

}  // namespace padhttp
