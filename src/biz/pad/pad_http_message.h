#ifndef PAD_HTTP_MESSAGE_H
#define PAD_HTTP_MESSAGE_H

// ============================================================================
// pad_http_message.h — Pad 接口 v1 HTTP 面的报文模型（解析 + §2.4 信封）
// ============================================================================
//
// 为什么自己写而不复用谁：
//   - Qt 5.12 没有 QHttpServer（那是 Qt 6.4 的 QtHttpServer 模块），本机工具链就是 5.12.8；
//   - 内嵌的 ZLMediaKit 只有 `mk_http_server_start`（静态文件）+ `on_mk_http_request` 钩子，
//     钩子给的 `mk_parser` 只暴露 method/url/header/content（见 3rdparty/zlmediakit/include/
//     mk_events_objects.h），**multipart 的文件 part 一样得自己切**；而它的监听口起来之后
//     不能重开（进程级一次，见 src/media/server/http_file_service.h 文件头），
//     调试页换端口就做不到。
//   - 静态文件那半仍然归 8090 那台文件服务（它有实测过的 Range/穿越/404 行为），
//     本文件只管 `/api/` 这一层的**报文形状**，不含监听、不含路由。
//
// 分层：本文件只依赖 QtCore（QByteArray/QString/QJsonObject），
// 不碰 socket，所以能被离线自测直接喂一段字节验证解析（tests/test_pad_http_message.cpp）。
//
// 三条刻意的口径，改动前先读：
//   1) **只认 Content-Length 与 chunked 两种正文**，两者都没有就当没有正文。
//      OkHttp/Retrofit 发 multipart 与 JSON 都带 Content-Length；chunked 是兜底，
//      不是"支持任意客户端"的承诺。
//   2) **上限先于解析**：请求行/头/正文各自有字节上限，超了立刻判失败，
//      不做"先收完再嫌大"。v1 §4.2 的照片上限（5 张 ×5MB）必须由调用方再校一遍，
//      这里的上限只是内存防线。
//   3) 解析失败**不区分**"哪一条规则没满足"以外的细节：错误文本是给人看的中文一句，
//      调用方一律回 400/2001（§2.6"字段缺失或类型错误 → 400/2001"）。
// ============================================================================

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>

#include <functional>

namespace padhttp {

// ============================================================================
// Part — multipart/form-data 的一个 part（§4.2 的 payload 与 photos 就是这个形状）
// ============================================================================
struct Part {
    QString name;            ///< Content-Disposition 的 name=，例 "payload" / "photos"
    QString filename;        ///< filename=；**非空即文件 part**
    QString contentType;     ///< 该 part 自己的 Content-Type，可空
    QByteArray data;         ///< part 正文原样字节
};

// ============================================================================
// Request — 一次已解析完的请求
// ============================================================================
struct Request {
    QString method;                       ///< 大写，例 "GET" / "POST"
    QString version;                      ///< 请求行第三段，例 "HTTP/1.1"（判 keep-alive 用）
    QString path;                         ///< 已百分号解码、不含 query
    QString target;                       ///< 请求行原样那一段（日志用，别再拿它判路径）
    QMap<QString, QString> query;         ///< 键值均已解码；同名键只留**第一个**
    QMap<QString, QString> headers;       ///< 键一律小写（HTTP 头名大小写不敏感）
    QByteArray body;                      ///< 原样字节（chunked 已还原）
    QString contentType;                  ///< 顶层 Content-Type，小写值里的 boundary 见 boundary
    QString boundary;                     ///< multipart 的 boundary；不是 multipart 时为空
    QList<Part> parts;                    ///< multipart 解析结果；非 multipart 为空
    QMap<QString, QString> pathParams;    ///< 路由 `{name}` 抓出来的段

    /// 头取值（大小写不敏感）。没有该头返回空串——v1 里没有"空串和缺失含义不同"的头，
    /// 所以不给 bool 出参；真需要区分（例如 Content-Length 是否出现）请看 decoder。
    QString header(const QString &name) const;
    /// query 取值。没有该键返回空串（同上：空串=没给）
    QString queryValue(const QString &key) const;
    /// 路由参数取值
    QString pathParam(const QString &key) const;

    /**
     * @brief 把 body 当 JSON 对象解出来
     * @param err [out] 中文失败原因（body 为空 / 不是合法 JSON / 顶层不是对象）
     * @return 解析好的对象；失败为空对象
     *
     * multipart 里的 `payload` part 也走这里（调用方把 part.data 拼成 QByteArray 再传）。
     * 顶层必须是对象：v1 所有写接口的 body 都是 `{...}`，数组/标量一律算类型错误。
     */
    static bool parseJsonObject(const QByteArray &raw, QJsonObject *out, QString *err);
    bool jsonBody(QJsonObject *out, QString *err) const;

    /// body 是否为 JSON（Content-Type 前缀判据）。只用于把"传错类型"和"传错字段"分开报
    bool isJsonContent() const;
};

// ============================================================================
// Reply — 一次要发回去的响应
// ============================================================================
struct Reply {
    int status = 200;                              ///< HTTP 状态码
    QByteArray body;                               ///< 正文原样字节
    QString contentType = QStringLiteral("application/json; charset=utf-8");
    QList<QPair<QString, QString>> extraHeaders;   ///< 追加响应头（顺序即写出顺序）
    bool forceClose = false;                       ///< true = 这次回复之后必须关连接
};

// ============================================================================
// 响应序列化
// ============================================================================
/// 状态码 → 原因短语。表里没有的状态回 "OK"/"Error" 之外的空短语仍然合法（RFC 允许）。
QString reasonPhrase(int status);

/// 把 Reply 拼成完整 HTTP 报文（含状态行、头、空行、正文）。
/// keepAlive=false 时写 `Connection: close`。Content-Length 一定写（本层不用 chunked 出站）。
QByteArray serializeReply(const Reply &reply, bool keepAlive);

// ============================================================================
// §2.4 信封
// ============================================================================
// 形状固定为 {code, message, detail, data}：detail 允许 null，data 允许 null。
// v1 的 data 在成功时基本都是对象，但 §4.4 之类没有"必须有键"的要求，所以入参用 QJsonValue。
QByteArray envelopeJson(int code, const QString &message, const QString &detail,
                        const QJsonValue &data);

/// code=0 / message="ok" / detail=null / data=…
Reply replyOk(const QJsonValue &data);
/// 通用错误出口：HTTP 状态与业务 code 分开传（§2.4 那张表就是这两列）
Reply replyError(int httpStatus, int code, const QString &message, const QString &detail);

// §2.4 表里每一档的便捷出口。message 是给 Pad 直接展示给用户的一句话（必填，中文），
// detail 只进日志、可为空串（空串时信封里写 null）。
Reply replyBadRequest(const QString &message, const QString &detail = QString());   ///< 400 / 2001
Reply replyUnauthorized(const QString &message, const QString &detail = QString()); ///< 401 / 1002
Reply replyNotFound(const QString &message, const QString &detail = QString());     ///< 404 / 2002
Reply replyConflict(const QString &message, const QString &detail = QString());     ///< 409 / 2003
/// 409 + 指定业务码：围栏那档 2004/2005/2006 都同一个 HTTP 状态、只有 code 不同
Reply replyConflictCode(int code, const QString &message, const QString &detail = QString());
Reply replyValidation(const QString &message, const QString &detail = QString());   ///< 422 / 4001
Reply replyInternal(const QString &message, const QString &detail = QString());     ///< 500 / 3001
Reply replyUnavailable(const QString &message, const QString &detail = QString());  ///< 503 / 3002

// ============================================================================
// 解析工具（暴露出来是为了让自测能逐条打，也为了服务端之外没有第二套判据）
// ============================================================================

/// 百分号解码（`+` 不当空格处理：v1 的 query 里只有 ID 与时间，没有表单空串口径）。
/// 非法转义（`%zz`、截断的 `%a`）**原样保留**那一段，不丢字节、不报错——
/// 报错会让一个畸形的 ID 变成"参数错误"，而它本该在业务层判"对象不存在"。
QString percentDecode(const QString &encoded);

/// 解析 query 串（不含前导 '?'）。同名键保留第一个。值已百分号解码。
QMap<QString, QString> parseQuery(const QString &rawQuery);

/// 从 Content-Type 里取 `boundary=`（带引号与不带引号都支持）。取不到返回空串。
QString multipartBoundary(const QString &contentType);

/**
 * @brief 解析 multipart/form-data 正文
 * @param body     正文原样字节
 * @param boundary 从 Content-Type 取出来的那个串
 * @param out      [out] 解析出的 part 列表（调用方负责清空）
 * @param err      [out] 中文失败原因
 * @param maxParts 最多允许几个 part（超了判失败，防"一个请求塞一万张图"）
 * @return false = 结构非法（缺定界、缺 CRLF、超上限）
 *
 * 只做 v1 §13.3.2 需要的那一档：定界行 `--boundary`、part 头到空行为止、正文到下一个
 * 定界前的 CRLF 为止。不支持 multipart 里嵌套 multipart，也不解码 part 内的编码
 * （binary 就是字节，Pad 发的 jpg/png/webp 都是二进制直传）。
 */
bool parseMultipart(const QByteArray &body, const QString &boundary,
                    QList<Part> *out, QString *err, int maxParts = 16);

// ============================================================================
// RequestDecoder — 增量喂字节的请求解码器（一条连接上一个，解析完可 reset 复用）
// ============================================================================
class RequestDecoder {
public:
    enum class State {
        ReadingHead,   ///< 还没收到结束空行
        ReadingBody,   ///< 头齐了，正文还在收
        Complete,      ///< 一个完整请求就绪，takeRequest() 能取
        Failed,        ///< 畸形或超限，连接必须关掉
    };

    RequestDecoder() = default;

    /// 字节上限。head = 请求行+头（含结束空行），body = 正文。都必须在 (0, 上限] 内。
    void setLimits(int maxHeadBytes, qint64 maxBodyBytes);

    /**
     * @brief 喂进 socket 刚收到的一段字节
     * @return false = 本段让请求变成畸形/超限（此时 state() 是 Failed，errorText() 有原因）
     *
     * 允许多次调用直到 Complete。Complete 之后再喂 = 下一次请求（keep-alive 流水线），
     * 但调用方应当先 takeRequest() 复位，否则这段会被并进上一个未取走的请求。
     */
    bool feed(const QByteArray &incoming);

    State state() const { return state_; }
    bool isComplete() const { return state_ == State::Complete; }
    bool isFailed() const { return state_ == State::Failed; }
    const QString &errorText() const { return error_; }

    /// 已收到的正文字节数（用于"收着收着超了上限"的日志）
    qint64 receivedBodyBytes() const { return body_.size(); }

    /**
     * @brief 取走解析好的请求并复位，准备解析同一连接上的下一个请求
     * @return false = 还没 Complete（或已 Failed）
     * @note 复位后会立刻继续消化内部缓冲里剩下的字节（客户端不等回复就连发下一个请求
     *       的情况），所以取走一个之后 isComplete() 可能直接是 true；推进失败只会把
     *       state 置成 Failed，调用方取干之后要看 isFailed()。
     */
    bool takeRequest(Request *out);

private:
    bool advance();            ///< 消化 buf_ 里已有的字节，把状态推到 Complete 或 Failed
    bool parseHead();          ///< 从 buf_ 里切出请求行与头，置 expectBody*
    bool consumeFixedBody();   ///< Content-Length 档：从 buf_ 往 body_ 搬
    bool consumeChunkedBody(); ///< chunked 档：状态机逐步推进
    void finishRequest();
    void fail(const QString &text);

    QByteArray buf_;            ///< 尚未消费的原始字节
    State state_ = State::ReadingHead;
    QString error_;

    // 已解析出的头部分
    QString method_;
    QString version_;
    QString target_;
    QString path_;
    QMap<QString, QString> query_;
    QMap<QString, QString> headers_;
    QString contentType_;
    QString boundary_;

    // 正文
    QByteArray body_;
    bool chunked_ = false;         ///< Transfer-Encoding 里有 chunked
    qint64 expectBytes_ = -1;      ///< Content-Length；-1 = 没有该头
    // chunked 子状态：0=等大小行 1=等正文 2=等正文后的 CRLF 3=等尾部头 4=收完
    int chunkPhase_ = 0;
    qint64 chunkRemaining_ = 0;

    int maxHeadBytes_ = 16384;
    qint64 maxBodyBytes_ = 26LL * 1024 * 1024;
};

}  // namespace padhttp

#endif  // PAD_HTTP_MESSAGE_H
