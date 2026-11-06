// ============================================================================
// native_fence_store.cpp — 本机围栏的 DB 读写与"每次编辑建新版"状态机
// ============================================================================
#include "native_fence_store.h"

#include "database_manager.h"

#include <QDateTime>
#include <QDebug>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace geofence {

namespace {

/// 与 pad_fence_store 同口径：定宽无偏移文本，出门口再补时区
QString internalNow()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

constexpr int kFirstFenceSeq = 1;

QSqlDatabase storeDb(const char *where)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen())
        qWarning() << "native_fence_store:" << where << "拿不到数据库连接（idge.db 没开）";
    return db;
}

} // namespace

// ============================================================================
// 读一条围栏的全部版本行（按版本号倒序）
// ============================================================================
QVector<PadFenceVersion> NativeFenceStore::loadVersions(const QString &fenceId) const
{
    QVector<PadFenceVersion> out;
    QSqlDatabase db = storeDb("loadVersions");
    if (!db.isOpen())
        return out;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT version, status, name, regions_json, frame_path, applied_at, "
        "       created_at, updated_at "
        "FROM native_fence_versions WHERE fence_id = :fence_id ORDER BY version DESC"));
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    if (!query.exec()) {
        qWarning() << "native_fence_store loadVersions failed:" << query.lastError().text();
        return out;
    }
    while (query.next()) {
        PadFenceVersion version;
        version.version = query.value(0).toInt();
        if (!padStatusFromName(query.value(1).toString(), &version.status)) {
            qWarning() << "native_fence_store 读到未知围栏状态，这一行已跳过："
                       << fenceId << query.value(1).toString();
            continue;
        }
        version.name = query.value(2).toString();
        QString regionError;
        if (!regionsFromJsonText(query.value(3).toString(), &version.regions, &regionError)) {
            qWarning() << "native_fence_store" << fenceId << "v" << version.version
                       << "的 regions 读不出来：" << regionError;
        }
        version.framePath = query.value(4).toString();
        version.appliedAt = query.value(5).toString();
        version.createdAt = query.value(6).toString();
        version.updatedAt = query.value(7).toString();
        // 本机围栏没有 plan/owner 快照（那是 Pad 下发围栏的概念），留空对象
        version.plan = QJsonObject();
        version.owner = QJsonObject();
        out.append(version);
    }
    return out;
}

// ============================================================================
// 取某 task 下的围栏（含 task_id='' 的全局历史桶）
// ============================================================================
// 迁移进来的老围栏没有 taskId，统一放在空串桶里；登录前/登录后都必须能读到，
// 否则"迁移成功"反而等于把围栏从判定里摘掉。同一 camera 两个桶都有时，当前
// task 的那条优先（它才是登录后新画的）。
QList<PadFence> NativeFenceStore::fencesForTask(const QString &taskId) const
{
    QList<PadFence> out;
    QSqlDatabase db = storeDb("fencesForTask");
    if (!db.isOpen())
        return out;
    // null 串绑成 SQL NULL 会让 `task_id = :task_id` 永不命中，统一按空串（全局桶）查
    const QString scopeTaskId = taskId.isNull() ? QStringLiteral("") : taskId;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT fence_id, task_id, camera_id, stream_id FROM native_fences "
        "WHERE task_id = :task_id OR task_id = '' "
        "ORDER BY CASE WHEN task_id = :task_id AND task_id <> '' THEN 0 ELSE 1 END, fence_id"));
    query.bindValue(QStringLiteral(":task_id"), scopeTaskId);
    if (!query.exec()) {
        qWarning() << "native_fence_store fencesForTask failed:" << query.lastError().text();
        return out;
    }
    QSet<QString> seenCameras;
    while (query.next()) {
        const QString cameraId = query.value(2).toString();
        if (seenCameras.contains(cameraId))
            continue;
        seenCameras.insert(cameraId);
        PadFence fence;
        fence.fenceId = query.value(0).toString();
        fence.taskId = query.value(1).toString();
        fence.cameraId = cameraId;
        fence.streamId = query.value(3).toString();
        fence.versions = loadVersions(fence.fenceId);
        out.append(fence);
    }
    return out;
}

// ============================================================================
// 按 (taskId, cameraId) 查主行
// ============================================================================
bool NativeFenceStore::loadFenceByTaskCamera(const QString &taskId, const QString &cameraId,
                                             PadFence *out) const
{
    QSqlDatabase db = storeDb("loadFenceByTaskCamera");
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT fence_id, task_id, camera_id, stream_id FROM native_fences "
        "WHERE task_id = :task_id AND camera_id = :camera_id"));
    query.bindValue(QStringLiteral(":task_id"), taskId);
    query.bindValue(QStringLiteral(":camera_id"), cameraId);
    if (!query.exec()) {
        qWarning() << "native_fence_store loadFenceByTaskCamera failed:" << query.lastError().text();
        return false;
    }
    if (!query.next())
        return false;
    if (!out)
        return false;
    out->fenceId = query.value(0).toString();
    out->taskId = query.value(1).toString();
    out->cameraId = query.value(2).toString();
    out->streamId = query.value(3).toString();
    out->versions = loadVersions(out->fenceId);
    return true;
}

// ============================================================================
// 取某 (taskId, cameraId) 的 ACTIVE 围栏
// ============================================================================
bool NativeFenceStore::loadActiveFence(const QString &taskId, const QString &cameraId,
                                       PadFence *out) const
{
    PadFence fence;
    if (!loadFenceByTaskCamera(taskId, cameraId, &fence))
        return false;
    // 只保留 ACTIVE 版本（本机正常情况下只有一条 ACTIVE）
    QVector<PadFenceVersion> active;
    for (const PadFenceVersion &v : fence.versions) {
        if (v.status == PadVersionStatus::Active)
            active.append(v);
    }
    if (active.isEmpty())
        return false;
    fence.versions = active;
    if (out)
        *out = fence;
    return true;
}

// ============================================================================
// 保存一个新版本为 ACTIVE（核心写路径）
// ============================================================================
// 本机没有 DRAFT：画完即生效。每次调用：
//   - 已有围栏：旧 ACTIVE → DISABLED，新建 version = max(version)+1 ACTIVE
//   - 无围栏：新建主行 + version=1 ACTIVE
// regions 里每个 region 的 regionId 由调用方保证已分配。
// 返回 fenceId；失败返回空串。
// ============================================================================
QString NativeFenceStore::saveActiveVersion(const QString &taskId, const QString &cameraId,
                                            const QString &streamId, const QString &name,
                                            const QVector<PadRegion> &regions) const
{
    QSqlDatabase db = storeDb("saveActiveVersion");
    if (!db.isOpen())
        return QString();

    // QString() 是 null 串，绑进 SQL 就是 NULL。task_id / stream_id 两列都是
    // NOT NULL：显式 NULL 不触发 DEFAULT ''，INSERT 会整条失败。迁移路径正是用
    // QString() 表示"全局空桶"，所以这里统一规范化成空串再读写。
    const QString scopeTaskId = taskId.isNull() ? QStringLiteral("") : taskId;
    const QString scopeStreamId = streamId.isNull() ? QStringLiteral("") : streamId;

    const QString now = internalNow();
    const QString regionsJson = regionsToJsonText(regions);

    PadFence existing;
    const bool hasExisting = loadFenceByTaskCamera(scopeTaskId, cameraId, &existing);

    // 1) 已有围栏：把旧 ACTIVE 置为 DISABLED
    if (hasExisting) {
        QSqlQuery disable(db);
        disable.prepare(QStringLiteral(
            "UPDATE native_fence_versions SET status = 'DISABLED', updated_at = :now "
            "WHERE fence_id = :fence_id AND status = 'ACTIVE'"));
        disable.bindValue(QStringLiteral(":now"), now);
        disable.bindValue(QStringLiteral(":fence_id"), existing.fenceId);
        if (!disable.exec()) {
            qWarning() << "native_fence_store disable old ACTIVE failed:"
                       << disable.lastError().text();
            return QString();
        }
    }

    // 2) 决定 fenceId 与 version
    QString fenceId;
    int version = 1;
    if (hasExisting) {
        fenceId = existing.fenceId;
        int maxV = 0;
        for (const PadFenceVersion &v : existing.versions)
            maxV = qMax(maxV, v.version);
        version = maxV + 1;
    } else {
        fenceId = nextFenceId();
        if (fenceId.isEmpty())
            return QString();
        // 插入主行
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral(
            "INSERT INTO native_fences (fence_id, task_id, camera_id, stream_id, "
            "                          created_at, updated_at) "
            "VALUES (:fence_id, :task_id, :camera_id, :stream_id, :now, :now)"));
        ins.bindValue(QStringLiteral(":fence_id"), fenceId);
        ins.bindValue(QStringLiteral(":task_id"), scopeTaskId);
        ins.bindValue(QStringLiteral(":camera_id"), cameraId);
        ins.bindValue(QStringLiteral(":stream_id"), scopeStreamId);
        ins.bindValue(QStringLiteral(":now"), now);
        if (!ins.exec()) {
            qWarning() << "native_fence_store insert fence failed:" << ins.lastError().text();
            return QString();
        }
    }

    // 3) 插入新 ACTIVE 版本行
    QSqlQuery ver(db);
    ver.prepare(QStringLiteral(
        "INSERT INTO native_fence_versions "
        "(fence_id, version, status, name, regions_json, applied_at, created_at, updated_at) "
        "VALUES (:fence_id, :version, 'ACTIVE', :name, :regions, :now, :now, :now)"));
    ver.bindValue(QStringLiteral(":fence_id"), fenceId);
    ver.bindValue(QStringLiteral(":version"), version);
    ver.bindValue(QStringLiteral(":name"), name);
    ver.bindValue(QStringLiteral(":regions"), regionsJson);
    ver.bindValue(QStringLiteral(":now"), now);
    if (!ver.exec()) {
        qWarning() << "native_fence_store insert version failed:" << ver.lastError().text();
        return QString();
    }

    // 4) 推主行 updated_at
    QSqlQuery touch(db);
    touch.prepare(QStringLiteral(
        "UPDATE native_fences SET updated_at = :now WHERE fence_id = :fence_id"));
    touch.bindValue(QStringLiteral(":now"), now);
    touch.bindValue(QStringLiteral(":fence_id"), fenceId);
    touch.exec();

    return fenceId;
}

// ============================================================================
// 诊断：库里本机围栏总数
// ============================================================================
int NativeFenceStore::fenceCount() const
{
    QSqlDatabase db = storeDb("fenceCount");
    if (!db.isOpen())
        return 0;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM native_fences")) || !query.next())
        return 0;
    return query.value(0).toInt();
}

// ============================================================================
// 分配下一个 fenceId：NFENCE-<自增号>，永不复用
// ============================================================================
QString NativeFenceStore::nextFenceId() const
{
    int maxSeq = kFirstFenceSeq - 1;
    QSqlDatabase db = storeDb("nextFenceId");
    if (db.isOpen()) {
        QSqlQuery query(db);
        if (query.exec(QStringLiteral("SELECT fence_id FROM native_fences"))) {
            while (query.next()) {
                const QString id = query.value(0).toString();
                const int dash = id.lastIndexOf(QLatin1Char('-'));
                if (dash >= 0) {
                    bool ok = false;
                    const int seq = id.mid(dash + 1).toInt(&ok);
                    if (ok)
                        maxSeq = qMax(maxSeq, seq);
                }
            }
        }
    }
    return QStringLiteral("NFENCE-%1").arg(maxSeq + 1, 3, 10, QLatin1Char('0'));
}

} // namespace geofence
