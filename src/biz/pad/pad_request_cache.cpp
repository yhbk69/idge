// ============================================================================
// pad_request_cache.cpp — §2.5 幂等表的读写与 runIdempotent
// ============================================================================
#include "pad_request_cache.h"

#include "database_manager.h"

#include <QDateTime>
#include <QDebug>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace pad {

namespace {

/// 库内那套定宽无偏移文本（与 alarms.alarm_time 同一口径，见 sql/idge_init.sql 文件头）。
/// 幂等表的清理靠它做字典序比较，所以必须是定宽——带偏移的新行会和老行混着比。
QString internalNow()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

/// requestId 的长度上限：UUID 是 36 个字符，64 已经宽松到能装下"带前缀的 UUID"，
/// 再长就该怀疑是调用方把整段报文塞进来了。
constexpr int kMaxRequestIdLength = 64;

}  // namespace

RequestIdCache::RequestIdCache(int retainHours)
    : retainHours_(qBound(1, retainHours, 24 * 30))   // 下限 1 小时、上限 30 天：
                                                     // 传 0 进去等于"存了立刻清掉"，
                                                     // 那是把幂等整条关掉却还留着表，最难查
{
}

bool RequestIdCache::lookup(const QString &requestId, CachedReply *out) const
{
    if (requestId.isEmpty() || !out)
        return false;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen())
        return false;   // 见头文件注释：库没开按"没命中"处理，不拒绝业务

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT http_status, content_type, body FROM pad_request_cache "
        "WHERE request_id = :request_id"));
    query.bindValue(QStringLiteral(":request_id"), requestId);
    if (!query.exec()) {
        qWarning() << "pad_request_cache lookup failed:" << query.lastError().text();
        return false;
    }
    if (!query.next())
        return false;

    out->status = query.value(0).toInt();
    out->contentType = query.value(1).toString();
    // body 存的是 TEXT；QSqlQuery 取回来是 QString，转字节必须显式 UTF-8，
    // 用 toLatin1 会把中文 message 打成问号，重放出去就成了另一条错误信息。
    out->body = query.value(2).toString().toUtf8();
    return true;
}

void RequestIdCache::store(const QString &requestId, const QString &route,
                           const padhttp::Reply &reply)
{
    if (requestId.isEmpty())
        return;   // 没有键就没法记；调用方（runIdempotent）已经在前面把空 requestId 判成 400 了
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        qWarning() << "pad_request_cache 没记：数据库未打开，本次写接口不受幂等保护" << route;
        return;
    }

    QSqlQuery query(db);
    // INSERT OR REPLACE：同一 requestId 走到这里只可能是"查过没有"，但并发/重启之间
    // 仍可能出现（两条连接几乎同时提交同一 requestId）， REPLACE 让它变成"后写的覆盖"
    // 而不是"整条 UPDATE 失败并给对端一个 500"。
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO pad_request_cache "
        "(request_id, route, http_status, content_type, body, created_at) "
        "VALUES (:request_id, :route, :http_status, :content_type, :body, :created_at)"));
    query.bindValue(QStringLiteral(":request_id"), requestId);
    query.bindValue(QStringLiteral(":route"), route);
    query.bindValue(QStringLiteral(":http_status"), reply.status);
    query.bindValue(QStringLiteral(":content_type"), reply.contentType);
    query.bindValue(QStringLiteral(":body"), QString::fromUtf8(reply.body));
    query.bindValue(QStringLiteral(":created_at"), internalNow());
    if (!query.exec()) {
        qWarning() << "pad_request_cache store failed:" << query.lastError().text();
        return;
    }
    query.finish();   // 带着活动语句进下一条 SQL（prune 的 DELETE）会挡住它

    prune();
}

int RequestIdCache::prune()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen())
        return 0;
    const QString cutoff = QDateTime::currentDateTime().addSecs(-3600LL * retainHours_)
                               .toString(Qt::ISODate);
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM pad_request_cache WHERE created_at < :cutoff"));
    query.bindValue(QStringLiteral(":cutoff"), cutoff);
    if (!query.exec()) {
        qWarning() << "pad_request_cache prune failed:" << query.lastError().text();
        return 0;
    }
    const int removed = query.numRowsAffected();
    query.finish();
    return removed;
}

int RequestIdCache::rowCount() const
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen())
        return 0;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM pad_request_cache")))
        return 0;
    if (!query.next())
        return 0;
    return query.value(0).toInt();
}

QString RequestIdCache::requireRequestId(const QJsonObject &body, QString *err)
{
    const QJsonValue value = body.value(QStringLiteral("requestId"));
    if (!value.isString() || value.toString().trimmed().isEmpty()) {
        if (err)
            *err = QString::fromUtf8("缺少 requestId（v1 §2.5 规定所有写接口必带）");
        return QString();
    }
    const QString requestId = value.toString().trimmed();
    if (requestId.size() > kMaxRequestIdLength) {
        if (err) {
            *err = QString::fromUtf8("requestId 超过 %1 个字符，本机不收")
                       .arg(kMaxRequestIdLength);
        }
        return QString();
    }
    return requestId;
}

padhttp::Reply runIdempotent(const QString &route, const QJsonObject &body,
                             const std::function<padhttp::Reply()> &work)
{
    QString error;
    const QString requestId = RequestIdCache::requireRequestId(body, &error);
    if (requestId.isEmpty())
        return padhttp::replyBadRequest(error, route);

    static RequestIdCache cache;
    CachedReply cached;
    if (cache.lookup(requestId, &cached)) {
        padhttp::Reply replay;
        replay.status = cached.status;
        replay.body = cached.body;
        if (!cached.contentType.isEmpty())
            replay.contentType = cached.contentType;
        // 命中幂等表**不进日志的 detail**：route 与 requestId 已经在 message 里，
        // 而 body 可能几千字节。日志文案由调用方按需补（各路由里那句"重复提交，返回首次结果"）。
        return replay;
    }

    const padhttp::Reply reply = work();
    cache.store(requestId, route, reply);
    return reply;
}

}  // namespace pad
