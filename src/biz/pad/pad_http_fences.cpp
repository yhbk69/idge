// ============================================================================
// pad_http_fences.cpp — v1 §5 围栏的六条 HTTP 路由
// ============================================================================
//
// 规约与实现的分工：状态组合与错误码的**判定**在 geofence::PadFenceStore
// （那里返回 DraftOutcome/ApplyOutcome…，不出现 HTTP 概念），
// 本文件只做三件事：把 JSON 解成入参并校验形状、把 outcome 翻成 §2.4 的信封、
// 把围栏对象按 §13.4 的形状序列化出去。
// 为什么要这么切：§5.3 那张状态组合表是这套接口里最容易改错的地方，
// 它必须能脱离 HTTP 被测（tests/test_pad_http_api.cpp 直接调 store 跑那九种组合）。
//
// outcome → 错误码的映射表（§5.3 + §2.4，一次列全，别在分支里各写一遍）：
//   FenceNotFound      → 404 / 2002   对象不存在
//   SourceVersionNotFound → 404 / 2002  带的那个版本号在这条围栏里没有
//   DraftAlreadyExists → 409 / 2004   该摄像头已存在草稿
//   NotDraftToEdit     → 409 / 2005   版本状态不允许该操作（没有可编辑的草稿）
//   WrongVersion       → 409 / 2005   apply/disable/delete-draft 指的版本状态不对
//   NotADraft/NotActive→ 409 / 2005   同上
//   CameraMismatch     → 409 / 2006   该摄像头已有围栏 / cameraId(taskId) 与围栏不一致
//   regions 非法        → 422 / 4001  业务校验失败（§5.3 与 §2.4 在这一档一致）
//   字段缺失或类型错    → 400 / 2001  （§2.6 那条总则）
//   StoreError         → 500 / 3001
//   画面渲染不出来      → 503 / 3003  存储/证据不可用（§5.3 要求 apply 必返 frameUrl）
//
// apply/disable 之后会调 FenceManager::refreshFromStores() 把这一版并进检测用的内存
// 快照（Pad 优先覆盖同通道的本机手绘版），所以对端收到 200/ACTIVE 时本机是真在判它。
// ⚠ 两张表的 cameraId **编号口径不同**：pad_fences 存的是对端原样发来的身份
//   （§7.1 的 channelIdentity() = "CH"+1-based），native_fences 存的是本机自己拼的
//   "CH"+0-based。所以列 native 围栏给对端前必须换成对外身份（nativeCameraIdForPad），
//   否则对端拿到的是它没法寻址的 "CH0"，而 "CH1" 在两边指两个通道。
// ============================================================================
#include "pad_http_api.h"

#include "alarm_uplink_engine.h"      // uplink::mediaUrlFor（"越界即不给"那份判据）
#include "ConfigManager.h"            // channelCount()：身份换算的越界判据
#include "fence_frame_renderer.h"
#include "fence_manager.h"   // apply/disable 之后要把 Pad 围栏并进检测用的内存快照
#include "pad_fence_store.h"
#include "native_fence_store.h"
#include "pad_http_server.h"
#include "pad_request_cache.h"
#include "pad_wire.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace pad {
namespace {

/// fenceId 的允许字符集。与 §4 的 alarmId 同一条理由：**它会进文件路径**
/// （fence-frames/<fenceId>/v<n>.png），不筛就是路径穿越。
bool isSafeFenceId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return id.size() <= 64 && re.match(id).hasMatch();
}

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

/// frameUrl：库里那条相对路径 → 能取到的地址。三条任一不满足就返回 null（不是空串）：
///   前缀或根为空（媒体服务没起来）、库里那条路径本来就是空（草稿必然如此）、
///   文件已经不在磁盘上（被清理/手工删过）。
/// §5.2 说"DRAFT 恒为 null"，§5.3 说 apply 缺 frameUrl 时 Pad 按失败处理——
/// 所以这里给 null 的两种情况语义完全不同，路由侧各自判（apply 那条走 503/3003）。
QJsonValue frameUrlJsonValue(const QString &relative, const HttpApiContext &ctx)
{
    if (relative.isEmpty() || ctx.mediaBaseUrl.isEmpty() || ctx.mediaRootDir.isEmpty())
        return QJsonValue(QJsonValue::Null);
    const QString absolute = QFileInfo(QDir(ctx.mediaRootDir).filePath(relative)).absoluteFilePath();
    if (!QFileInfo::exists(absolute))
        return QJsonValue(QJsonValue::Null);
    const QString url = uplink::mediaUrlFor(ctx.mediaBaseUrl, ctx.mediaRootDir, absolute);
    if (url.isEmpty())
        return QJsonValue(QJsonValue::Null);
    return QJsonValue(url);
}

/// §13.4.2 的一个版本对象。时间在这里才补时区偏移（库里是定宽无偏移文本）。
QJsonObject versionJson(const geofence::PadFenceVersion &version, const HttpApiContext &ctx)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("version"), version.version);
    obj.insert(QStringLiteral("status"), geofence::padStatusName(version.status));
    obj.insert(QStringLiteral("name"), version.name);
    obj.insert(QStringLiteral("regions"), geofence::regionsJson(version.regions));
    obj.insert(QStringLiteral("frameUrl"), frameUrlJsonValue(version.framePath, ctx));
    obj.insert(QStringLiteral("appliedAt"), version.appliedAt.isEmpty()
        ? QJsonValue(QJsonValue::Null) : QJsonValue(pad::wireTime(version.appliedAt)));
    obj.insert(QStringLiteral("createdAt"), pad::wireTime(version.createdAt));
    obj.insert(QStringLiteral("updatedAt"), pad::wireTime(version.updatedAt));
    // plan/owner 是**快照**：存进去什么形状就出什么形状（§13.4.4/§13.4.5 的键由 Pad 决定，
        // 本机不补字段、不改名、不重排——重排会让对端按文本比较时看出差异）
    obj.insert(QStringLiteral("plan"), version.plan);
    obj.insert(QStringLiteral("owner"), version.owner);
    return obj;
}

/**
 * @brief native_fences 的 cameraId → 对端能寻址的身份（§7.1 口径）
 *
 * 本机手绘那张表存的是 "CH"+**0-based** 通道号（persistChannelLocked 自己拼的），
 * 而 §6/§7.1 对外报的身份是 "CH"+**1-based**。直接把库里的字符串回给对端有两个后果：
 *   - 第 0 路的手绘围栏回出去是 "CH0"，而对端从来没见过、也没法用这个 id 寻址；
 *   - 第 1 路的回出去是 "CH1"，在对端那边指的是**第 0 路**——它照着这个通道再画一条，
 *     于是同一路上挂着两条围栏，而且它以为自己在改本机那条。
 * 认不出来（不是 CHn、或换算后越界）就原样回，宁可不换也不编一个假身份。
 */
QString nativeCameraIdForPad(const QString &nativeCameraId)
{
    static const QRegularExpression kNativeCameraId(QStringLiteral("^CH([0-9][0-9]?)$"));
    const QRegularExpressionMatch m = kNativeCameraId.match(nativeCameraId.trimmed());
    if (!m.hasMatch())
        return nativeCameraId;
    const int zeroBased = m.captured(1).toInt();
    if (zeroBased < 0 || zeroBased >= ConfigManager::instance().channelCount())
        return nativeCameraId;
    return QStringLiteral("CH%1").arg(zeroBased + 1);
}

/// §13.4.1 的围栏对象
QJsonObject fenceJson(const geofence::PadFence &fence, const HttpApiContext &ctx){
    QJsonObject obj;
    obj.insert(QStringLiteral("fenceId"), fence.fenceId);
    obj.insert(QStringLiteral("taskId"), fence.taskId);
    obj.insert(QStringLiteral("cameraId"), fence.cameraId);
    obj.insert(QStringLiteral("streamId"), fence.streamId);
    obj.insert(QStringLiteral("activeVersion"), fence.activeVersion());
    obj.insert(QStringLiteral("draftVersion"), fence.draftVersion());
    QJsonArray versions;
    for (const geofence::PadFenceVersion &version : fence.versions)
        versions.append(versionJson(version, ctx));
    obj.insert(QStringLiteral("versions"), versions);   // 恒数组，空也是 []（§2.6）
    return obj;
}

/// DraftOutcome → 信封
padhttp::Reply draftFailureReply(geofence::PadFenceStore::DraftOutcome outcome,
                                 const QString &detail)
{
    using Outcome = geofence::PadFenceStore::DraftOutcome;
    switch (outcome) {
    case Outcome::FenceNotFound:
        return padhttp::replyNotFound(QString::fromUtf8("本机没有这条围栏"), detail);
    case Outcome::SourceVersionNotFound:
        return padhttp::replyNotFound(QString::fromUtf8("这条围栏里没有要复制的那个版本"), detail);
    case Outcome::DraftAlreadyExists:
        return padhttp::replyConflictCode(2004,
            QString::fromUtf8("这个摄像头已经有一份草稿了，请先下发或删除草稿"), detail);
    case Outcome::NotDraftToEdit:
        return padhttp::replyConflictCode(2005,
            QString::fromUtf8("这条围栏现在没有可编辑的草稿"), detail);
    case Outcome::CameraMismatch:
        return padhttp::replyConflictCode(2006,
            QString::fromUtf8("这个摄像头已经有围栏了，不能重复创建；"
                              "或请求里的 cameraId/taskId 与围栏不一致"), detail);
    case Outcome::Created:
    case Outcome::Edited:
    case Outcome::Copied:
        // 成功不该走到这里。真走到了就是路由的逻辑写反了——回 500 而不是假装成功。
        return padhttp::replyInternal(QString::fromUtf8("内部错误：save-draft 成功分支走进了失败映射"),
                                      detail);
    case Outcome::StoreError:
        break;
    }
    return padhttp::replyInternal(QString::fromUtf8("围栏没写进库"), detail);
}

/// apply/disable/delete-draft 三条的公共前置：fenceId 合法 + body 是对象 + requestId/版本齐
bool parseFenceVersionBody(const padhttp::Request &request, QString *fenceIdOut,
                           QJsonObject *body, int *versionOut,
                           padhttp::Reply *reject)
{
    const QString fenceId = request.pathParam(QStringLiteral("fenceId"));
    if (!isSafeFenceId(fenceId)) {
        *reject = padhttp::replyBadRequest(QString::fromUtf8("fenceId 写法不合法"),
                                          QStringLiteral("fenceId=%1").arg(fenceId.left(80)));
        return false;
    }
    QString parseError;
    if (!request.jsonBody(body, &parseError)) {
        *reject = padhttp::replyBadRequest(QString::fromUtf8("请求体不是合法的 JSON 对象"),
                                           parseError);
        return false;
    }
    // §13.5：这三条都只要 requestId + version 两个键，其它键一律忽略（§2.6"未知字段必须忽略"）
    QString requestIdError;
    if (RequestIdCache::requireRequestId(*body, &requestIdError).isEmpty()) {
        *reject = padhttp::replyBadRequest(requestIdError, fenceId);
        return false;
    }
    const QJsonValue versionValue = body->value(QStringLiteral("version"));
    if (!versionValue.isDouble()) {
        *reject = padhttp::replyBadRequest(QString::fromUtf8("缺少必填字段 version（整数）"),
                                           QStringLiteral("§13.5"));
        return false;
    }
    *fenceIdOut = fenceId;
    *versionOut = static_cast<int>(versionValue.toDouble());
    return true;
}

}  // namespace

void registerFenceRoutes(PadHttpServer *server, const HttpApiContext &ctx)
{
    if (!server)
        return;

    geofence::PadFenceStore store;

    // ── §5.3 GET /fences?taskId=... ────────────────────────────────────
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/fences"),
                     [store, ctx](const padhttp::Request &request) -> padhttp::Reply {
                         const QString taskId = request.queryValue(QStringLiteral("taskId"));
                         if (taskId.isEmpty()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少查询参数 taskId"),
                                 QStringLiteral("GET /api/fences?taskId=<施工计划 ID>"));
                         }
                         QJsonArray fences;
                         // 1) Pad 下发的围栏（pad_fences）
                         const QList<geofence::PadFence> list = store.fencesForTask(taskId);
                         QSet<QString> padCameraIds;
                         for (const geofence::PadFence &fence : list) {
                             fences.append(fenceJson(fence, ctx));
                             padCameraIds.insert(fence.cameraId);
                         }
                         // 2) 本机画的围栏（native_fences）：同一**物理通道**被 Pad 占用则跳过（Pad 优先）
                         //    比的是换算成对外身份之后的 cameraId——直接比字符串会漏掉全部撞车，
                         //    因为本机那套是 0-based、Pad 那套是 1-based（见 nativeCameraIdForPad）。
                         geofence::NativeFenceStore nativeStore;
                         const QList<geofence::PadFence> nativeList = nativeStore.fencesForTask(taskId);
                         for (const geofence::PadFence &fence : nativeList) {
                             geofence::PadFence view = fence;   // 只改这份拷贝，库里不动
                             view.cameraId = nativeCameraIdForPad(fence.cameraId);
                             // §13.4.1 里 streamId 是必填，而 channelIdentity() 给对端的
                             // streamId 与 cameraId 同值；本机手绘那张表的 streamId 是**空串**
                             // （persistChannelLocked 传的就是 QString()），照原样出等于回一个
                             // 空必填字段，所以这里跟着身份一起补上。
                             view.streamId = view.cameraId;
                             if (padCameraIds.contains(view.cameraId))
                                 continue;
                             fences.append(fenceJson(view, ctx));
                         }
                         QJsonObject data;
                         data.insert(QStringLiteral("taskId"), taskId);
                         data.insert(QStringLiteral("fences"), fences);
                         return padhttp::replyOk(data);
                     });

    // ── §5.3 GET /api/fences/{fenceId} ─────────────────────────────────
    server->addRoute(QStringLiteral("GET"), QStringLiteral("/api/fences/{fenceId}"),
                     [store, ctx](const padhttp::Request &request) -> padhttp::Reply {
                         const QString fenceId = request.pathParam(QStringLiteral("fenceId"));
                         if (!isSafeFenceId(fenceId)) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("fenceId 写法不合法"),
                                 QStringLiteral("fenceId=%1").arg(fenceId.left(80)));
                         }
                         geofence::PadFence fence;
                         if (!store.loadFence(fenceId, &fence)) {
                             return padhttp::replyNotFound(QString::fromUtf8("本机没有这条围栏"),
                                                           QStringLiteral("fenceId=%1").arg(fenceId));
                         }
                         const QJsonObject obj = fenceJson(fence, ctx);
                         QJsonObject data;
                         data.insert(QStringLiteral("fence"), obj);
                         // data.versions 就是 fence.versions 的同一份拷贝（§5.3）。
                         // 两处不一致时以 fence 为准——所以这里直接取上面那个对象里的数组，
                         // 保证两份**永远逐字节相同**，而不是各读一次库。
                         data.insert(QStringLiteral("versions"),
                                     obj.value(QStringLiteral("versions")).toArray());
                         return padhttp::replyOk(data);
                     });

    // ── §5.3 POST /api/fences/save-draft ───────────────────────────────
    server->addRoute(QStringLiteral("POST"), QStringLiteral("/api/fences/save-draft"),
                     [store](const padhttp::Request &request) -> padhttp::Reply {
                         QJsonObject body;
                         QString parseError;
                         if (!request.jsonBody(&body, &parseError))
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("请求体不是合法的 JSON 对象"), parseError);
                         QString error;
                         const QString requestId = RequestIdCache::requireRequestId(body, &error);
                         if (requestId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§5.3 save-draft"));
                         geofence::PadFenceStore::DraftInput input;
                         input.fenceId = body.value(QStringLiteral("fenceId")).toString().trimmed();
                         // fenceId 会进画面文件的路径，带了就必须合法（不带是新建，无所谓）
                         if (!input.fenceId.isEmpty() && !isSafeFenceId(input.fenceId)) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("fenceId 写法不合法"),
                                 QStringLiteral("fenceId=%1").arg(input.fenceId.left(80)));
                         }
                         input.taskId = requireText(body, QStringLiteral("taskId"), &error);
                         if (input.taskId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§13.5"));
                         input.cameraId = requireText(body, QStringLiteral("cameraId"), &error);
                         if (input.cameraId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§13.5"));
                         input.streamId = requireText(body, QStringLiteral("streamId"), &error);
                         if (input.streamId.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§13.5"));
                         input.name = requireText(body, QStringLiteral("name"), &error);
                         if (input.name.isEmpty())
                             return padhttp::replyBadRequest(error, QStringLiteral("§13.5"));
                         if (!body.value(QStringLiteral("plan")).isObject()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 plan（对象）"),
                                 QStringLiteral("§13.4.4"));
                         }
                         if (!body.value(QStringLiteral("owner")).isObject()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 owner（对象）"),
                                 QStringLiteral("§13.4.5"));
                         }
                         // owner.id 与 plan.taskId 是这两张快照里唯一的必填项（§13.4.4/§13.4.5）
                         if (body.value(QStringLiteral("owner")).toObject()
                                 .value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("owner.id 不能为空"), QStringLiteral("§13.4.5"));
                         }
                         if (body.value(QStringLiteral("plan")).toObject()
                                 .value(QStringLiteral("taskId")).toString().trimmed().isEmpty()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("plan.taskId 不能为空"), QStringLiteral("§13.4.4"));
                         }
                         // plan.taskId 与顶层 taskId 不等时不报错也不改写：
                         // 那两条各有用途（顶层是围栏的归属，plan 里那份是当时的快照原文），
                         // 本机替谁都不对。等对方给口径，这条也进了待确认清单。
                         if (!body.value(QStringLiteral("regions")).isArray()) {
                             return padhttp::replyBadRequest(
                                 QString::fromUtf8("缺少必填字段 regions（数组）"),
                                 QStringLiteral("§13.4.3"));
                         }
                         QString regionError;
                         if (!geofence::validateRegions(
                                 body.value(QStringLiteral("regions")).toArray(), &input.regions,
                                 &regionError)) {
                             return padhttp::replyValidation(
                                 QString::fromUtf8("围栏区域不合法"), regionError);
                         }
                         input.plan = body.value(QStringLiteral("plan")).toObject();
                         input.owner = body.value(QStringLiteral("owner")).toObject();
                         const QJsonValue sourceValue =
                             body.value(QStringLiteral("sourceVersion"));
                         if (sourceValue.isDouble()) {
                             input.hasSourceVersion = true;
                             input.sourceVersion = static_cast<int>(sourceValue.toDouble());
                         }

                         return runIdempotent(
                             QStringLiteral("POST /api/fences/save-draft"), body,
                             [&]() -> padhttp::Reply {
                                 QString fenceIdOut;
                                 int versionOut = 0;
                                 QString detail;
                                 const auto outcome = store.saveDraft(input, &fenceIdOut,
                                                                      &versionOut, &detail);
                                 if (outcome != geofence::PadFenceStore::DraftOutcome::Created
                                     && outcome != geofence::PadFenceStore::DraftOutcome::Edited
                                     && outcome != geofence::PadFenceStore::DraftOutcome::Copied) {
                                     return draftFailureReply(outcome, detail);
                                 }
                                 // §5.3"保存/下发响应为扁平结果，不含形状"——
                                 // 这里只回四个键，不把 regions 再吐回去一遍
                                 QJsonObject data;
                                 data.insert(QStringLiteral("fenceId"), fenceIdOut);
                                 data.insert(QStringLiteral("version"), versionOut);
                                 data.insert(QStringLiteral("status"), QStringLiteral("DRAFT"));
                                 geofence::PadFence fence;
                                 QString updatedAt;
                                 if (store.loadFence(fenceIdOut, &fence)) {
                                     geofence::PadFenceVersion version;
                                     if (fence.findVersion(versionOut, &version))
                                         updatedAt = version.updatedAt;
                                 }
                                 data.insert(QStringLiteral("updatedAt"),
                                             pad::wireTime(updatedAt));   // 空串兜底成"现在"
                                 return padhttp::replyOk(data);
                             });
                     });

    // ── §5.3 POST /api/fences/{fenceId}/apply ──────────────────────────
    server->addRoute(QStringLiteral("POST"), QStringLiteral("/api/fences/{fenceId}/apply"),
                     [store, ctx](const padhttp::Request &request) -> padhttp::Reply {
                         QString fenceId;
                         QJsonObject body;
                         int version = 0;
                         padhttp::Reply reject;
                         if (!parseFenceVersionBody(request, &fenceId, &body, &version, &reject))
                             return reject;

                         return runIdempotent(
                             QStringLiteral("POST /api/fences/{id}/apply"), body,
                             [&]() -> padhttp::Reply {
                                 // 画面**先渲染**再改状态：反过来会出现
                                 // "库里已经 ACTIVE 了但 frameUrl 给不出"，
                                 // 而 §5.3 明写那种情况 Pad 按失败处理——
                                 // 失败就该什么都没发生，重试才安全。
                                 if (ctx.fenceFrameDir.isEmpty() || ctx.mediaBaseUrl.isEmpty()
                                         || ctx.mediaRootDir.isEmpty()) {
                                     return padhttp::replyError(
                                         503, 3003,
                                         QString::fromUtf8("本机现在出不了围栏生效画面"
                                                           "（画面渲染没开，或媒体服务地址没配）"),
                                         QStringLiteral("fence_frames=%1")
                                             .arg(ctx.fenceFrameDir.isEmpty() ? "off" : "on"));
                                 }
                                 geofence::PadFence fence;
                                 if (!store.loadFence(fenceId, &fence)) {
                                     return padhttp::replyNotFound(
                                         QString::fromUtf8("本机没有这条围栏"),
                                         QStringLiteral("fenceId=%1").arg(fenceId));
                                 }
                                 geofence::PadFenceVersion target;
                                 if (!fence.findVersion(version, &target)) {
                                     return padhttp::replyConflictCode(
                                         2005, QString::fromUtf8("这条围栏里没有那个版本"),
                                         QStringLiteral("fenceId=%1 version=%2")
                                             .arg(fenceId).arg(version));
                                 }
                                 if (target.status != geofence::PadVersionStatus::Draft) {
                                     return padhttp::replyConflictCode(
                                         2005, QString::fromUtf8("只有草稿能下发"),
                                         QStringLiteral("status=%1")
                                             .arg(geofence::padStatusName(target.status)));
                                 }
                                 QString absolutePath;
                                 QString relativePath;
                                 QString renderError;
                                 if (!geofence::renderFenceFrame(fenceId, version, target.regions,
                                                                 ctx.mediaRootDir,
                                                                 ctx.fenceFrameDir, &absolutePath,
                                                                 &relativePath, &renderError)) {
                                     return padhttp::replyError(503, 3003,
                                                                QString::fromUtf8("围栏生效画面渲染不出来"),
                                                                renderError);
                                 }
                                 QString appliedAt;
                                 QString detail;
                                 const auto outcome = store.applyDraft(fenceId, version, &appliedAt,
                                                                       &detail);
                                 if (outcome != geofence::PadFenceStore::ApplyOutcome::Applied) {
                                     if (outcome == geofence::PadFenceStore::ApplyOutcome::FenceNotFound)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("本机没有这条围栏"), detail);
                                     if (outcome == geofence::PadFenceStore::ApplyOutcome::WrongVersion)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("这条围栏里没有那个版本"), detail);
                                     if (outcome == geofence::PadFenceStore::ApplyOutcome::NotADraft)
                                         return padhttp::replyConflictCode(
                                             2005, QString::fromUtf8("只有草稿能下发"), detail);
                                     return padhttp::replyInternal(QString::fromUtf8("围栏下发没写进库"),
                                                                   detail);
                                 }
                                 if (!store.setFramePath(fenceId, version, relativePath)) {
                                     // 状态已经改了、画面路径没落库：这比一开始就失败更难解释，
                                     // 所以照实回 3001 并在 detail 里说清哪一步没成。
                                     qWarning() << "apply 成功但 frame_path 落库失败：" << fenceId
                                                << version;
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("已下发，但生效画面没记进库"),
                                         QStringLiteral("setFramePath failed"));
                                 }
                                 QJsonObject data;
                                 data.insert(QStringLiteral("fenceId"), fenceId);
                                 data.insert(QStringLiteral("version"), version);
                                 data.insert(QStringLiteral("status"), QStringLiteral("ACTIVE"));
                                 // 让这一版**立刻参与检测**：检测读的是 FenceManager 的内存
                                 // 快照，以前 apply 只写 pad_fences、没人刷新那份快照，
                                 // 于是对端收到 200/ACTIVE 而本机一帧都不判，要到下次
                                 // 登录/登出或重启才偶然生效。
                                 geofence::FenceManager::instance().refreshFromStores();
                                 data.insert(QStringLiteral("appliedAt"), pad::wireTime(appliedAt));
                                 const QJsonValue frameUrl =
                                     frameUrlJsonValue(relativePath, ctx);
                                 if (frameUrl.isNull()) {
                                     // 刚写完的文件立刻取不到 = 媒体根与前缀不匹配，
                                     // 这属于配置错，宁可有响出声，也不回一个取不到的地址了事。
                                     return padhttp::replyError(
                                         503, 3003,
                                         QString::fromUtf8("生效画面已生成，但地址给不出来"),
                                         QStringLiteral("path=%1 base=%2")
                                             .arg(relativePath, ctx.mediaBaseUrl));
                                 }
                                 data.insert(QStringLiteral("frameUrl"), frameUrl);
                                 return padhttp::replyOk(data);
                             });
                     });

    // ── §5.3 POST /api/fences/{fenceId}/disable ────────────────────────
    server->addRoute(QStringLiteral("POST"), QStringLiteral("/api/fences/{fenceId}/disable"),
                     [store](const padhttp::Request &request) -> padhttp::Reply {
                         QString fenceId;
                         QJsonObject body;
                         int version = 0;
                         padhttp::Reply reject;
                         if (!parseFenceVersionBody(request, &fenceId, &body, &version, &reject))
                             return reject;
                         return runIdempotent(
                             QStringLiteral("POST /api/fences/{id}/disable"), body,
                             [&]() -> padhttp::Reply {
                                 QString disabledAt;
                                 QString detail;
                                 const auto outcome =
                                     store.disableVersion(fenceId, version, &disabledAt, &detail);
                                 if (outcome != geofence::PadFenceStore::DisableOutcome::Disabled) {
                                     if (outcome == geofence::PadFenceStore::DisableOutcome::FenceNotFound)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("本机没有这条围栏"), detail);
                                     if (outcome == geofence::PadFenceStore::DisableOutcome::WrongVersion)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("这条围栏里没有那个版本"), detail);
                                     if (outcome == geofence::PadFenceStore::DisableOutcome::NotActive)
                                         return padhttp::replyConflictCode(
                                             2005, QString::fromUtf8("只有生效版本能停用"), detail);
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("停用没写进库"), detail);
                                 }
                                 QJsonObject data;
                                 data.insert(QStringLiteral("fenceId"), fenceId);
                                 data.insert(QStringLiteral("version"), version);
                                 data.insert(QStringLiteral("status"), QStringLiteral("DISABLED"));
                                 // 停用同样要刷新：不刷新的话这一路还在按刚被停掉的那版报警。
                                 // 该通道若还有本机手绘的围栏，refresh 会把它并回来
                                 // （Pad 优先的规则只在 Pad 有 ACTIVE 版本时成立）。
                                 geofence::FenceManager::instance().refreshFromStores();
                                 data.insert(QStringLiteral("disabledAt"), pad::wireTime(disabledAt));
                                 return padhttp::replyOk(data);
                             });
                     });

    // ── §5.3 POST /api/fences/{fenceId}/delete-draft ───────────────────
    server->addRoute(QStringLiteral("POST"),
                     QStringLiteral("/api/fences/{fenceId}/delete-draft"),
                     [store](const padhttp::Request &request) -> padhttp::Reply {
                         QString fenceId;
                         QJsonObject body;
                         int version = 0;
                         padhttp::Reply reject;
                         if (!parseFenceVersionBody(request, &fenceId, &body, &version, &reject))
                             return reject;
                         return runIdempotent(
                             QStringLiteral("POST /api/fences/{id}/delete-draft"), body,
                             [&]() -> padhttp::Reply {
                                 QString detail;
                                 const auto outcome = store.deleteDraft(fenceId, version, &detail);
                                 if (outcome != geofence::PadFenceStore::DeleteOutcome::Deleted) {
                                     if (outcome == geofence::PadFenceStore::DeleteOutcome::FenceNotFound)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("本机没有这条围栏"), detail);
                                     if (outcome == geofence::PadFenceStore::DeleteOutcome::WrongVersion)
                                         return padhttp::replyNotFound(
                                             QString::fromUtf8("这条围栏里没有那个版本"), detail);
                                     if (outcome == geofence::PadFenceStore::DeleteOutcome::NotADraft)
                                         return padhttp::replyConflictCode(
                                             2005, QString::fromUtf8("只有草稿能删除"), detail);
                                     return padhttp::replyInternal(
                                         QString::fromUtf8("删除草稿没写成"), detail);
                                 }
                                 QJsonObject data;
                                 data.insert(QStringLiteral("fenceId"), fenceId);
                                 data.insert(QStringLiteral("version"), version);
                                 data.insert(QStringLiteral("deleted"), true);
                                 return padhttp::replyOk(data);
                             });
                     });
}

}  // namespace pad
