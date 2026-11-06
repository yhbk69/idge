// ============================================================================
// pad_http_alarms.cpp — v1 §4 报警处置面（处置选项 / 处置 / 误报 / 已读）
// ============================================================================
//
// 四条路由的共同形状：验字段 → 查这条报警存不存在、是不是终态 → 落库（经 AlarmManager，
// 不是直接调 DAO）→ 用 runIdempotent() 把首次结果记进 §2.5 的幂等表。
// "经 AlarmManager"这一条不是洁癖：内存窗口 alarms_ 里那份副本必须在同一次动作里一起改，
// 否则关机时的 syncToDatabase() 会把旧状态写回去，处置在第二天早上被静默撤销。
//
// §2.4 那张错误码表与 §2.6/§4.2 之间有一处冲突，这里定了一份可执行的解法
// （口径确认单里那条"400 还是 422"的歧义就是这个）：
//   - **字段缺失或类型错** → 400 / 2001。依据 §2.6 那句
//     "字段缺失或类型错误时，边缘设备返回 400/2001（HTTP）或 error 2001（WS）"，
//     它是对**所有**接口说的，且 §2.4 表里 400 那行只对应 2001。
//   - **值域/数量不合规**（照片超 5 张、单张超 5MB、measure 空串）→ 400 / 4001。
//     依据 §4.2 正文那句"照片超过 5 张/单张超 5MB → 400 / 4001"。
//     ⚠ §2.4 表里 4001 只出现在 HTTP 422 那一行，所以"400 + 4001"是规约自己给的组合，
//       这里按 §4.2 的字面执行而不是"顺手改成 422"：Pad 判的是 code 还是状态码没写在文档里，
//       而 §4.2 是唯一一处把两者配在一起说清的地方。
//   - **业务校验失败**（围栏形状非法、区域数超限）→ 422 / 4001，与 §2.4 表一致。
// 这三档分别对应 replyBadRequest / replyBadRequest(code=4001) / replyValidation，
// 中间那一档没有现成的便捷函数，所以本文件里用 replyError(400, 4001, …) 显式写。
//
// syncState 恒 `SUBMITTED`：§4.2 明写它"仅表示边缘设备的服务器同步状态"，
// 而本机现在**没有**向服务器上传处置的链路（alarm_uplink 那条是报警出站，不含处置回执），
// 所以 SUBMITTED（边缘已接收、上传未完成）是唯一诚实的值。
// 报 UPLOADED 会让 Pad 显示"已上传服务器"，那是假数。
// ============================================================================
#include "pad_http_api.h"

#include "ConfigManager.h"
#include "alarm_dao.h"
#include "alarm_manager.h"
#include "alarm_labels.h"   // §4.1 内置处置选项的唯一一份表（本机详情框共用）
#include "alarm_uplink_engine.h"   // 只借 uplink::mediaUrlFor 那份"越界即不给"的实现
#include "pad_http_server.h"
#include "pad_request_cache.h"
#include "pad_wire.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDebug>
#include <QFileInfo>
#include <QVector>

#include <algorithm>

namespace pad {
namespace {

/// 单张整改照片的字节上限（§4.2 明写 5MB）
constexpr qint64 kMaxPhotoBytes = 5LL * 1024 * 1024;
/// 整改照片张数上限（§4.2 明写 0~5 张）
constexpr int kMaxPhotoCount = 5;

QJsonObject optionJson(qint64 id, const QString &label, int enabled, int sortOrder)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), static_cast<double>(id));   // §13.3.1 是 long
    obj.insert(QStringLiteral("label"), label);
    obj.insert(QStringLiteral("enabled"), enabled);
    obj.insert(QStringLiteral("sortOrder"), sortOrder);
    return obj;
}

/// §4.1 的内置选项：**直接取 alarm_labels::disposeOptions() 那一张表**（2026-11-06 起）。
/// 以前这里自己写两条、本机详情框再写一套，两端就分叉了；处置单选组现在共用一份，
/// 改措辞只改 src/biz/alarm/alarm_labels.cpp 那一处。
/// 现场要换文案走 pad_http/dispose_options 配置（下面 disposeOptions() 的整组覆盖）。
void appendBuiltInOptions(QJsonArray *out)
{
    for (const alarm_labels::DisposeOption &one : alarm_labels::disposeOptions())
        out->append(optionJson(one.id, one.label, 1, one.id));
}

/// alarmId 的允许字符集：库里那列是 32 位十六进制 UUID，但历史/联调数据可能有别的写法，
/// 所以放到 [A-Za-z0-9_-]、长度 ≤64。
/// **这条判据不是装饰**：alarmId 会拼进照片的落盘路径，不筛就是路径穿越
/// （`POST /api/alarms/..%2f..%2fetc%2fdispose/x/read` 这种）。
bool isSafeAlarmId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return id.size() <= 64 && re.match(id).hasMatch();
}

/// 从 Content-Type 猜照片扩展名。**只用 MIME 决定扩展名，绝不用 filename**：
/// multipart 的 filename 完全由对端控制，拿它拼路径等于让 Pad 决定本机往哪写文件。
QString extensionForMime(const QString &mime)
{
    if (mime.contains(QStringLiteral("jpeg"), Qt::CaseInsensitive)
        || mime.contains(QStringLiteral("jpg"), Qt::CaseInsensitive))
        return QStringLiteral(".jpg");
    if (mime.contains(QStringLiteral("png"), Qt::CaseInsensitive))
        return QStringLiteral(".png");
    if (mime.contains(QStringLiteral("webp"), Qt::CaseInsensitive))
        return QStringLiteral(".webp");
    // MIME 没给或不认识时按 §4.2 允许的第一档 jpg 落盘。
    // 为什么不"按对端给的扩展名"：那是同一个路径穿越问题；
    // 为什么不"拒收"：Pad 的 Retrofit 有时只给 application/octet-stream，
    // 拒收会让现场根本处置不了，而字节本身是原样存的，格式不影响看图。
    return QStringLiteral(".jpg");
}

/**
 * @brief §4.1 的选项来源：配置整组替换，空数组用内置
 * @note 配置里的每一项都校验：label 非空 + id 是数字。不合法的**跳过**而不是让接口 500——
 *       一个拼错的键不该让 Pad 连处置选项都拉不到（拉不到它就不伪造选项，界面直接空）。
 *       全被跳过时回落内置，理由同上。
 */
QJsonArray disposeOptions(const QString &alarmType)
{
    Q_UNUSED(alarmType);   // 见下面这段注释
    const QJsonArray configured = ConfigManager::instance().padHttpDisposeOptions();
    QJsonArray out;
    if (configured.isEmpty()) {
        appendBuiltInOptions(&out);
        return out;
    }
    int order = 1;
    for (const QJsonValue &item : configured) {
        const QJsonObject obj = item.toObject();
        const QString label = obj.value(QStringLiteral("label")).toString().trimmed();
        if (label.isEmpty() || !obj.value(QStringLiteral("id")).isDouble()) {
            qWarning() << "pad_http.dispose_options 里有一项不合法（缺 id 或 label），已跳过："
                       << QJsonDocument(obj).toJson(QJsonDocument::Compact);
            continue;
        }
        out.append(optionJson(
            static_cast<qint64>(obj.value(QStringLiteral("id")).toDouble()), label,
            obj.value(QStringLiteral("enabled")).toInt(1) == 0 ? 0 : 1,
            obj.value(QStringLiteral("sortOrder")).toInt(order)));
        ++order;
    }
    if (out.isEmpty()) {   // 配置写了但全被跳过 → 还是给内置那几条，不给空数组
        appendBuiltInOptions(&out);
    }
    // 按 sortOrder 升序（§4.1"按 sortOrder 排序"）。sortOrder 缺省已在上面按数组顺序补过，
    // 所以这里稳定排序就够。
    // ⚠ 不能直接对 QJsonArray 用 std::sort：它的迭代器解出来是 QJsonValueRef，
    //   那个代理类型没有可交换语义（实测 gcc13 在 stl_algobase 里就报"no matching swap"）。
    //   所以先落到 QVector<QJsonObject> 排完再装回数组——多一次拷贝，换的是编译得过。
    QVector<QJsonObject> sorted;
    sorted.reserve(out.size());
    for (const QJsonValue &item : out)
        sorted.append(item.toObject());
    std::sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("sortOrder")).toInt()
             < b.value(QStringLiteral("sortOrder")).toInt();
    });
    QJsonArray ordered;
    for (const QJsonObject &one : sorted)
        ordered.append(one);
    return ordered;
}

/// measure 文本 → §4.1 选项 id 的反查。§4.2 的 multipart 里只有 measure 这段人看的文本，
/// 规约没给选项 id 字段，所以对端提交能对上哪条 label 就按那条的 id 记进
/// dispose_option_id；对不上（Pad 写了自由文本）返回 0 = 这一列不动。
/// 有这一列的理由：措辞以后要改，改了不至于让"已现场纠正"的历史统计对不到今天。
int optionIdForLabel(const QString &measure)
{
    const QString want = measure.trimmed();
    for (const QJsonValue &item : disposeOptions(QString())) {
        const QJsonObject obj = item.toObject();
        // enabled=0 的项不参与反查：配置里关掉一条，意思是"这条以后不该再被选中"，
        // 而历史文本恰好还是那句措辞时不该把它的 id 记成一次新的处置。
        if (obj.value(QStringLiteral("enabled")).toInt(1) == 0)
            continue;
        if (obj.value(QStringLiteral("label")).toString().trimmed() == want)
            return static_cast<int>(obj.value(QStringLiteral("id")).toDouble());
    }
    return 0;
}

/// 从 body 里取一个必填字符串字段。空串与"没这个键"都算缺失（§2.6：可选字段才允许 null）。
/// 返回空串并把原因写进 err，调用方直接 replyBadRequest 即可。
QString requireText(const QJsonObject &body, const QString &key, QString *err)
{
    const QString value = body.value(key).toString().trimmed();
    if (value.isEmpty()) {
        if (err)
            *err = QString::fromUtf8("缺少必填字段 %1").arg(key);
        return QString();
    }
    return value;
}

/// 落一条报警的当前状态，供"存不存在 / 是不是终态"两条前置判定共用
enum class AlarmPresence { Missing, Open, Terminal };

AlarmPresence probeAlarm(const QString &alarmId, AlarmRecord *record)
{
    AlarmDAO dao;
    *record = dao.queryById(alarmId);
    if (record->id.isEmpty())
        return AlarmPresence::Missing;
    if (record->status == QLatin1String("disposed")
        || record->status == QLatin1String("false_alarm"))
        return AlarmPresence::Terminal;
    return AlarmPresence::Open;
}

/// 把 §4.2 的处置记录拼成响应里的 dispose 对象（与 §13.2.4 同一形状，WS 那边共用字段名）
QJsonObject disposeObjectJson(const QString &measure, const QString &remark,
                              const QString &personId, const QString &personName,
                              const QString &disposedAtWire, const QJsonArray &photoUrls)
{
    QJsonObject dispose;
    dispose.insert(QStringLiteral("measure"), measure);
    dispose.insert(QStringLiteral("remark"), remark);
    if (!personId.isEmpty())
        dispose.insert(QStringLiteral("personId"), personId);
    dispose.insert(QStringLiteral("personName"), personName);
    dispose.insert(QStringLiteral("disposedAt"), disposedAtWire);
    dispose.insert(QStringLiteral("photoUrls"), photoUrls);
    return dispose;
}

/// 从请求里取出那份 JSON payload。multipart 走 `payload` part，直投 JSON 也认（见注释）。
bool extractPayload(const padhttp::Request &request, QJsonObject *body, padhttp::Reply *reject)
{
    if (!request.boundary.isEmpty()) {
        for (const padhttp::Part &part : request.parts) {
            if (part.name != QLatin1String("payload"))
                continue;
            QString parseError;
            if (!padhttp::Request::parseJsonObject(part.data, body, &parseError)) {
                *reject = padhttp::replyBadRequest(
                    QString::fromUtf8("payload 部分不是合法的 JSON 对象"), parseError);
                return false;
            }
            return true;
        }
        *reject = padhttp::replyBadRequest(QString::fromUtf8("multipart 里缺少名为 payload 的部分"),
                                          QStringLiteral("payload"));
        return false;
    }
    // 非 multipart：只接受 application/json。
    // 为什么容忍这一档而不是严格按 §4.2 只收 multipart：Pad 照规约发 multipart，
    // 而现场排障、自测、以及"对端想先用 curl 验一遍字段规则"都是直投 JSON——
    // 两条路最终都汇到同一个 parseJsonObject、同一套校验，不存在第二套规则。
    QString parseError;
    if (!request.jsonBody(body, &parseError)) {
        *reject = padhttp::replyBadRequest(QString::fromUtf8("请求体不是合法的 JSON 对象"),
                                           parseError);
        return false;
    }
    return true;
}

}  // namespace

void registerAlarmRoutes(PadHttpServer *server, const HttpApiContext &ctx)
{
    if (!server)
        return;

    // ── §4.1 GET /api/dispose-options?alarmType=HELMET ──────────────────
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/dispose-options"),
                     [](const padhttp::Request &request) -> padhttp::Reply {
                         // alarmType 在 §13.3.1 是可选查询参数（"用于返回该报警类型的处置选项"）。
                         // 本机**不按类型分档**：处置选项是现场措辞，不是每台设备每类报警一套，
                         // 而现在也没有那个来源。所以这里收下、原样忽略，所有类型回同一组。
                         // ⚠ 这条已写进 README 的"待对方确认"清单：Pad 若指望按类型给不同选项，
                         //   需要先在配置里给出分档数据，改的只是这个函数。
                         QJsonObject data;
                         data.insert(QStringLiteral("options"),
                                     disposeOptions(request.queryValue(QStringLiteral("alarmType"))));
                         return padhttp::replyOk(data);
                     });

    // ── §4.2 POST /api/alarms/{alarmId}/dispose ─────────────────────────
    server->addRoute(QStringLiteral("POST"),
                     QStringLiteral("/api/alarms/{alarmId}/dispose"),
                     [ctx](const padhttp::Request &request) -> padhttp::Reply {
                         const QString alarmId = request.pathParam(QStringLiteral("alarmId"));
                         if (!isSafeAlarmId(alarmId)) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("alarmId 写法不合法"),
                                 QStringLiteral("alarmId=%1").arg(alarmId.left(80)));
                         }
                         QJsonObject body;
                         padhttp::Reply reject;
                         if (!extractPayload(request, &body, &reject))
                             return reject;

                         QString error;
                         const QString requestId = RequestIdCache::requireRequestId(body, &error);
                         if (requestId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§4.2"));
                         const QString taskId = body.value(QStringLiteral("taskId")).toString().trimmed();
                         if (taskId.isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 taskId"), QStringLiteral("§4.2"));
                         const QString measure = requireText(body, QStringLiteral("measure"), &error);
                         if (measure.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§4.2"));
                         const QString remark = body.value(QStringLiteral("remark")).toString().trimmed();
                         const QString personId =
                             body.value(QStringLiteral("personId")).toString().trimmed();
                         const QString personName =
                             body.value(QStringLiteral("personName")).toString().trimmed();
                         const QString disposedAtWire =
                             body.value(QStringLiteral("disposedAt")).toString().trimmed();
                         if (disposedAtWire.isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 disposedAt"),
                                 QStringLiteral("§4.2"));
                         // 对端那个带偏移的瞬间换算成库内那套定宽文本（口径见 pad_wire.h）。
                         // 解不出来不直接判错：让 AlarmManager 用本机时刻兜底，
                         // 比"因为 Pad 的时钟格式写法不同而永远处置不掉"更可用。
                         const QString disposedAt = pad::internalTimeFromWire(disposedAtWire);

                         // ── 照片先数、再逐张判大小 ──
                         QList<const padhttp::Part *> photoParts;
                         for (const padhttp::Part &part : request.parts) {
                             if (part.name == QLatin1String("photos"))
                                 photoParts.append(&part);
                         }
                         if (photoParts.size() > kMaxPhotoCount) {
                             // 这一档是"数量/大小不合规"，按文件头那份解法回 400 + 4001
                             return padhttp::replyError(
                                 400, 4001,
                                 QString::fromUtf8("整改照片最多 %1 张").arg(kMaxPhotoCount),
                                 QStringLiteral("photos=%1").arg(photoParts.size()));
                         }
                         for (const padhttp::Part *part : photoParts) {
                             if (static_cast<qint64>(part->data.size()) > kMaxPhotoBytes) {
                                 return padhttp::replyError(
                                     400, 4001,
                                     QString::fromUtf8("单张整改照片不能超过 5MB"),
                                     QStringLiteral("photos=%1 bytes").arg(part->data.size()));
                             }
                         }

                         AlarmRecord record;
                         const AlarmPresence checked = probeAlarm(alarmId, &record);
                         if (checked == AlarmPresence::Missing) {
                             return padhttp::replyNotFound(
                                 QString::fromUtf8("本机没有这条报警"),
                                 QStringLiteral("alarmId=%1").arg(alarmId));
                         }
                         if (checked == AlarmPresence::Terminal) {
                             return padhttp::replyConflict(
                                 QString::fromUtf8("这条报警已经是终态，不能再次处置"),
                                 QStringLiteral("status=%1").arg(record.status));
                         }

                         return runIdempotent(
                             QStringLiteral("POST /api/alarms/{id}/dispose"), body,
                             [&]() -> padhttp::Reply {
                                 // 照片落盘：路径完全由本机决定（<媒体根>/dispose/<alarmId>/<序号>），
                                 // 对端给的 filename 一个字都不参与，防的就是路径穿越。
                                 QStringList savedPaths;
                                 if (!ctx.mediaRootDir.isEmpty() && !photoParts.isEmpty()) {
                                     const QString dir = QDir(ctx.mediaRootDir)
                                             .filePath(QStringLiteral("dispose/%1").arg(alarmId));
                                     if (!QDir().mkpath(dir)) {
                                         return padhttp::replyError(
                                             503, 3003,
                                             QString::fromUtf8("整改照片存不下"),
                                             QStringLiteral("mkpath %1").arg(dir));
                                     }
                                     int index = 1;
                                     for (const padhttp::Part *part : photoParts) {
                                         const QString file = QDir(dir).filePath(
                                             QStringLiteral("dispose%1%2")
                                                 .arg(index)
                                                 .arg(extensionForMime(part->contentType)));
                                         QFile out(file);
                                         if (!out.open(QIODevice::WriteOnly)
                                                 || out.write(part->data) != part->data.size()) {
                                             return padhttp::replyError(
                                                 503, 3003,
                                                 QString::fromUtf8("整改照片写盘失败"),
                                                 QStringLiteral("%1: %2")
                                                     .arg(file, out.errorString()));
                                         }
                                         out.close();
                                         // 库里存**相对媒体根**的路径，出 URL 时才拼前缀
                                         savedPaths << QStringLiteral("dispose/%1/%2")
                                                           .arg(alarmId)
                                                           .arg(QFileInfo(file).fileName());
                                         ++index;
                                     }
                                 }
                                 QJsonArray photoUrls;
                                 const QString rootDir = ctx.mediaRootDir;
                                 const QString baseUrl = ctx.mediaBaseUrl;
                                 for (const QString &relative : savedPaths) {
                                     const QString url = uplink::mediaUrlFor(
                                         baseUrl, rootDir,
                                         QFileInfo(QDir(rootDir).filePath(relative)).absoluteFilePath());
                                     if (!url.isEmpty())
                                         photoUrls.append(url);
                                 }

                                 QJsonArray paths;
                                 for (const QString &relative : savedPaths)
                                     paths.append(relative);
                                 QString disposeError;
                                 if (!AlarmManager::instance().disposeAlarmById(
                                         alarmId, measure, remark, personId, personName,
                                         QString::fromUtf8(QJsonDocument(paths).toJson(
                                             QJsonDocument::Compact)),
                                         disposedAt, optionIdForLabel(measure), &disposeError)) {
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("处置没写进库"), disposeError);
                                 }

                                 AlarmRecord after;
                                 probeAlarm(alarmId, &after);   // 读回本机真正落库的那份
                                 QJsonObject data;
                                 data.insert(QStringLiteral("alarmId"), alarmId);
                                 data.insert(QStringLiteral("status"), pad::wireStatus(after));
                                 data.insert(QStringLiteral("dispose"), disposeObjectJson(
                                     measure, remark, personId, personName,
                                     pad::wireTime(after.disposeTime), photoUrls));
                                 data.insert(QStringLiteral("updatedAt"),
                                             pad::wireTime(after.updateTime));
                                 // 恒 SUBMITTED：本机没有"把处置上传到服务器"的链路，见文件头
                                 data.insert(QStringLiteral("syncState"), QStringLiteral("SUBMITTED"));
                                 return padhttp::replyOk(data);
                             });
                     });

    // ── §4.3 POST /api/alarms/{alarmId}/false-alarm ─────────────────────
    server->addRoute(QStringLiteral("POST"),
                     QStringLiteral("/api/alarms/{alarmId}/false-alarm"),
                     [](const padhttp::Request &request) -> padhttp::Reply {
                         const QString alarmId = request.pathParam(QStringLiteral("alarmId"));
                         if (!isSafeAlarmId(alarmId)) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("alarmId 写法不合法"),
                                 QStringLiteral("alarmId=%1").arg(alarmId.left(80)));
                         }
                         QJsonObject body;
                         padhttp::Reply reject;
                         if (!extractPayload(request, &body, &reject))
                             return reject;

                         QString error;
                         const QString requestId = RequestIdCache::requireRequestId(body, &error);
                         if (requestId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§4.3"));
                         if (body.value(QStringLiteral("taskId")).toString().trimmed().isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 taskId"), QStringLiteral("§4.3"));
                         const QString markedAtWire =
                             body.value(QStringLiteral("markedAt")).toString().trimmed();
                         if (markedAtWire.isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 markedAt"), QStringLiteral("§4.3"));
                         const QString markedAt = pad::internalTimeFromWire(markedAtWire);
                         const QString personId =
                             body.value(QStringLiteral("personId")).toString().trimmed();
                         const QString personName =
                             body.value(QStringLiteral("personName")).toString().trimmed();

                         AlarmRecord record;
                         const AlarmPresence presence = probeAlarm(alarmId, &record);
                         if (presence == AlarmPresence::Missing) {
                             return padhttp::replyNotFound(
                                 QString::fromUtf8("本机没有这条报警"),
                                 QStringLiteral("alarmId=%1").arg(alarmId));
                         }
                         if (presence == AlarmPresence::Terminal) {
                             return padhttp::replyConflict(
                                 QString::fromUtf8("这条报警已经是终态，不能再次标记"),
                                 QStringLiteral("status=%1").arg(record.status));
                         }

                         return runIdempotent(
                             QStringLiteral("POST /api/alarms/{id}/false-alarm"), body,
                             [&]() -> padhttp::Reply {
                                 if (!AlarmManager::instance().markAsFalsePositiveById(
                                         alarmId, personId, personName, markedAt)) {
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("误报没写进库"),
                                         QStringLiteral("alarmId=%1").arg(alarmId));
                                 }
                                 AlarmRecord after;
                                 probeAlarm(alarmId, &after);
                                 QJsonObject data;
                                 data.insert(QStringLiteral("alarmId"), alarmId);
                                 data.insert(QStringLiteral("status"), QStringLiteral("FALSE_ALARM"));
                                 // §13.3.3 的 disposedAt 在这里语义是"误报时间"，
                                 // 库里落的就是 dispose_time，同一个列、同一个字段名，不再另算一份
                                 data.insert(QStringLiteral("disposedAt"),
                                             pad::wireTime(after.disposeTime));
                                 data.insert(QStringLiteral("updatedAt"),
                                             pad::wireTime(after.updateTime));
                                 return padhttp::replyOk(data);
                             });
                     });

    // ── §4.4 POST /api/alarms/{alarmId}/read ────────────────────────────
    server->addRoute(QStringLiteral("POST"),
                     QStringLiteral("/api/alarms/{alarmId}/read"),
                     [](const padhttp::Request &request) -> padhttp::Reply {
                         const QString alarmId = request.pathParam(QStringLiteral("alarmId"));
                         if (!isSafeAlarmId(alarmId)) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("alarmId 写法不合法"),
                                 QStringLiteral("alarmId=%1").arg(alarmId.left(80)));
                         }
                         QJsonObject body;
                         padhttp::Reply reject;
                         if (!extractPayload(request, &body, &reject))
                             return reject;

                         QString error;
                         const QString requestId = RequestIdCache::requireRequestId(body, &error);
                         if (requestId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§4.4"));
                         if (body.value(QStringLiteral("taskId")).toString().trimmed().isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 taskId"), QStringLiteral("§4.4"));
                         const QString readAtWire =
                             body.value(QStringLiteral("readAt")).toString().trimmed();
                         if (readAtWire.isEmpty())
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 readAt"), QStringLiteral("§4.4"));
                         const QString readAt = pad::internalTimeFromWire(readAtWire);

                         AlarmRecord record;
                         const AlarmPresence presence = probeAlarm(alarmId, &record);
                         if (presence == AlarmPresence::Missing) {
                             return padhttp::replyNotFound(
                                 QString::fromUtf8("本机没有这条报警"),
                                 QStringLiteral("alarmId=%1").arg(alarmId));
                         }
                         // 终态**不**挡：§4.4 说的是"第一次打开详情"，已处置的报警照样会被打开看，
                         // 而"读过"与"处置过"是两列（read_time / status），互不覆盖。

                         return runIdempotent(
                             QStringLiteral("POST /api/alarms/{id}/read"), body,
                             [&]() -> padhttp::Reply {
                                 if (!AlarmManager::instance().markAlarmReadById(alarmId, readAt)) {
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("已读没写进库"),
                                         QStringLiteral("alarmId=%1").arg(alarmId));
                                 }
                                 AlarmRecord after;
                                 probeAlarm(alarmId, &after);
                                 QJsonObject data;
                                 data.insert(QStringLiteral("alarmId"), alarmId);
                                 // 回的是**生效的那个**已读时刻：库里已有的更早时这里就是老的，
                                 // §13.3.4 的"生效的最早已读时间"就是这个意思
                                 data.insert(QStringLiteral("readAt"), pad::wireTime(after.readTime));
                                 data.insert(QStringLiteral("updatedAt"),
                                             pad::wireTime(after.updateTime));
                                 return padhttp::replyOk(data);
                             });
                     });

    Q_UNUSED(ctx);
}

}  // namespace pad
