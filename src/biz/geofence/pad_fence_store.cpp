// ============================================================================
// pad_fence_store.cpp — v1 §5 围栏的两张表读写与 §5.3 状态机
// ============================================================================
// 与规约的对应关系全部写在函数注释上；文件头那三条"为什么这么存"见 pad_fence_store.h。
// ============================================================================
#include "pad_fence_store.h"

#include "database_manager.h"

#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace geofence {

namespace {

/// 库里那套定宽无偏移文本（与 alarms.alarm_time 同一口径，见 sql/idge_init.sql 文件头）。
/// §5.2 的 createdAt/updatedAt 出门口再补时区（pad_http_fences.cpp），库里一律存这个形状。
QString internalNow()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

/// fenceId 的编号地板：现表里一个都没有时从 1 起，而不是 0。
constexpr int kFirstFenceSeq = 1;

QSqlDatabase storeDb(const char *where)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen())
        qWarning() << "pad_fence_store:" << where << "拿不到数据库连接（idge.db 没开）";
    return db;
}

}  // namespace

// ============================================================================
// 状态名
// ============================================================================

QString padStatusName(PadVersionStatus status)
{
    switch (status) {
    case PadVersionStatus::Active:   return QStringLiteral("ACTIVE");
    case PadVersionStatus::Draft:    return QStringLiteral("DRAFT");
    case PadVersionStatus::Disabled: return QStringLiteral("DISABLED");
    }
    // 三个枚举值全举完到这里，正常编译不会走到。留一条兜底而不是删掉：
    // 哪天加了第四个状态忘了改 switch，返回一个不可能的名字比"未定义行为"好排查。
    return QStringLiteral("DRAFT");
}

bool padStatusFromName(const QString &text, PadVersionStatus *out)
{
    if (text == QLatin1String("ACTIVE")) {
        if (out) *out = PadVersionStatus::Active;
        return true;
    }
    if (text == QLatin1String("DRAFT")) {
        if (out) *out = PadVersionStatus::Draft;
        return true;
    }
    if (text == QLatin1String("DISABLED")) {
        if (out) *out = PadVersionStatus::Disabled;
        return true;
    }
    return false;   // 库里读到脏值：调用方跳过那一行并告警，绝不"就当草稿"猜一个
}

// ============================================================================
// Region ↔ JSON
// ============================================================================

QJsonObject PadRegion::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("regionId"), regionId);
    obj.insert(QStringLiteral("shapeType"), shapeType);
    QJsonArray pointArray;
    for (const QPointF &point : points) {
        // 每个点写回**两个数**，且按 double 出：规约 §13.4.3 是 array<array<number>>。
        // 归一化值 0~1 用 QJsonValue(double) 会写成 0.5/0.25 这种最短形式，
        // 与对端发来的字符串可能差在末尾零上——那是 JSON 数字的正常表现，
        // 对端按数值比较，不是按文本比较，所以不必为此存原文。
        QJsonArray pair;
        pair.append(point.x());
        pair.append(point.y());
        pointArray.append(pair);
    }
    obj.insert(QStringLiteral("points"), pointArray);
    obj.insert(QStringLiteral("ruleType"), ruleType);
    return obj;
}

QJsonArray regionsJson(const QVector<PadRegion> &regions)
{
    QJsonArray out;
    for (const PadRegion &region : regions)
        out.append(region.toJson());
    return out;
}

QString regionsToJsonText(const QVector<PadRegion> &regions)
{
    return QString::fromUtf8(QJsonDocument(regionsJson(regions)).toJson(QJsonDocument::Compact));
}

bool regionsFromJsonText(const QString &text, QVector<PadRegion> *out, QString *err)
{
    if (out)
        out->clear();
    if (text.trimmed().isEmpty()) {
        if (err)
            *err = QString::fromUtf8("regions_json 是空的");
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isArray()) {
        if (err)
            *err = QString::fromUtf8("regions_json 不是数组");
        return false;
    }
    // 读回来的这一路**不再跑一遍 validateRegions**：那八条是对**入参**的约束，
    // 库里既然有就说明当时过过；重跑一遍反而会让"当年放进去的宽松数据"读不出来。
    // 形状坏（缺键/非数组点）就跳过那一个区域并告警，其余照读。
    for (const QJsonValue &item : doc.array()) {
        const QJsonObject obj = item.toObject();
        PadRegion region;
        region.regionId = obj.value(QStringLiteral("regionId")).toString();
        region.shapeType = obj.value(QStringLiteral("shapeType")).toString();
        region.ruleType = obj.value(QStringLiteral("ruleType")).toString();
        const QJsonArray points = obj.value(QStringLiteral("points")).toArray();
        for (const QJsonValue &point : points) {
            const QJsonArray pair = point.toArray();
            if (pair.size() < 2)
                continue;
            region.points.append(QPointF(pair.at(0).toDouble(), pair.at(1).toDouble()));
        }
        if (out)
            out->append(region);
    }
    return true;
}

// ============================================================================
// §5.2 的 regions 校验
// ============================================================================

bool validateRegions(const QJsonArray &raw, QVector<PadRegion> *out, QString *err)
{
    if (out)
        out->clear();
    // 条数：0 与 >8 都拒（§5.3"regions 为 0 个或超过 8 个 → 422/4001"）
    if (raw.isEmpty()) {
        if (err) *err = QString::fromUtf8("regions 是空的，至少要一个区域");
        return false;
    }
    if (raw.size() > 8) {
        if (err)
            *err = QString::fromUtf8("regions 超过 8 个（现在是 %1 个）").arg(raw.size());
        return false;
    }

    QStringList seenIds;
    QVector<PadRegion> parsed;
    for (int i = 0; i < raw.size(); ++i) {
        const QJsonValue &item = raw.at(i);
        if (!item.isObject()) {
            if (err) *err = QString::fromUtf8("第 %1 个区域不是对象").arg(i + 1);
            return false;
        }
        const QJsonObject obj = item.toObject();
        PadRegion region;
        // regionId：必填、非空、同一次请求内不重复。本机**不生成不改写**（§5.2 原文）。
        region.regionId = obj.value(QStringLiteral("regionId")).toString().trimmed();
        if (region.regionId.isEmpty()) {
            if (err) *err = QString::fromUtf8("第 %1 个区域缺 regionId").arg(i + 1);
            return false;
        }
        if (seenIds.contains(region.regionId)) {
            if (err)
                *err = QString::fromUtf8("regionId 重复：%1").arg(region.regionId);
            return false;
        }
        seenIds.append(region.regionId);

        region.shapeType = obj.value(QStringLiteral("shapeType")).toString().trimmed().toUpper();
        if (region.shapeType != QLatin1String("RECT") && region.shapeType != QLatin1String("POLYGON")) {
            if (err)
                *err = QString::fromUtf8("第 %1 个区域的 shapeType 只认 RECT/POLYGON，来的是 %2")
                           .arg(i + 1).arg(region.shapeType);
            return false;
        }
        // ruleType 同理只有两档（§13.4.3）。同一次里可以混用，所以不按全局判。
        region.ruleType = obj.value(QStringLiteral("ruleType")).toString().trimmed().toUpper();
        if (region.ruleType != QLatin1String("INTRUSION")
            && region.ruleType != QLatin1String("CROSS")) {
            if (err)
                *err = QString::fromUtf8("第 %1 个区域的 ruleType 只认 INTRUSION/CROSS，来的是 %2")
                           .arg(i + 1).arg(region.ruleType);
            return false;
        }

        const QJsonArray points = obj.value(QStringLiteral("points")).toArray();
        const int count = points.size();
        if (region.shapeType == QLatin1String("RECT")) {
            // RECT 恰好 4 点（§13.4.3）。注意这与本机自己的围栏不一样：
            // geofence::FenceShape 的矩形是**两个对角点**，v1 传的是四个顶点。
            // 这里按 v1 存四个点，不折算成两角点——折了再折回来就有顺序歧义。
            if (count != 4) {
                if (err)
                    *err = QString::fromUtf8("RECT 必须恰好 4 个顶点，第 %1 个区域给了 %2 个")
                               .arg(i + 1).arg(count);
                return false;
            }
        } else if (count < 3 || count > 20) {
            if (err)
                *err = QString::fromUtf8("POLYGON 需要 3~20 个顶点，第 %1 个区域给了 %2 个")
                           .arg(i + 1).arg(count);
            return false;
        }
        for (int p = 0; p < count; ++p) {
            const QJsonArray pair = points.at(p).toArray();
            if (pair.size() < 2 || !pair.at(0).isDouble() || !pair.at(1).isDouble()) {
                if (err)
                    *err = QString::fromUtf8("第 %1 个区域的第 %2 个点不是 [x, y] 两个数")
                               .arg(i + 1).arg(p + 1);
                return false;
            }
            const double x = pair.at(0).toDouble();
            const double y = pair.at(1).toDouble();
            // 归一化 0~1：越界就拒而不是 clamp。clamp 会把"画到画面外"这个错误
            // 静默变成"贴着边的一条围栏"，现场看不出来，判定却已经不一样了。
            if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                if (err)
                    *err = QString::fromUtf8(
                              "第 %1 个区域的第 %2 个点坐标超出 0~1：(%3, %4)")
                               .arg(i + 1).arg(p + 1).arg(x).arg(y);
                return false;
            }
            region.points.append(QPointF(x, y));
        }
        parsed.append(region);
    }
    if (out)
        *out = parsed;
    return true;
}

// ============================================================================
// PadFence 的小计算
// ============================================================================

int PadFence::activeVersion() const
{
    for (const PadFenceVersion &version : versions) {
        if (version.status == PadVersionStatus::Active)
            return version.version;
    }
    return 0;   // §13.4.1：没有就是 0，不是 -1、也不是"字段省略"
}

int PadFence::draftVersion() const
{
    for (const PadFenceVersion &version : versions) {
        if (version.status == PadVersionStatus::Draft)
            return version.version;
    }
    return 0;
}

bool PadFence::findVersion(int version, PadFenceVersion *out) const
{
    for (const PadFenceVersion &one : versions) {
        if (one.version == version) {
            if (out)
                *out = one;
            return true;
        }
    }
    return false;
}

// ============================================================================
// 读
// ============================================================================

QVector<PadFenceVersion> PadFenceStore::loadVersions(const QString &fenceId) const
{
    QVector<PadFenceVersion> out;
    QSqlDatabase db = storeDb("loadVersions");
    if (!db.isOpen())
        return out;
    QSqlQuery query(db);
    // ⚠ **不**加 LIMIT：§5.2 要"全部版本、按版本号倒序"，一次给全。
    //    一个围栏的版本数是个位数（只有复制才涨号），没有分页的必要。
    query.prepare(QStringLiteral(
        "SELECT version, status, name, regions_json, plan_json, owner_json, "
        "       frame_path, applied_at, created_at, updated_at "
        "FROM pad_fence_versions WHERE fence_id = :fence_id ORDER BY version DESC"));
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    if (!query.exec()) {
        qWarning() << "pad_fence_store loadVersions failed:" << query.lastError().text();
        return out;
    }
    while (query.next()) {
        PadFenceVersion version;
        version.version = query.value(0).toInt();
        if (!padStatusFromName(query.value(1).toString(), &version.status)) {
            // 脏状态名：跳过这一行而不是猜。宁可让 Pad 看到一个少了某版本的围栏，
            // 也别把一个 DISABLED 当草稿让它再编辑一遍（那是终态不可编辑的反面）。
            qWarning() << "pad_fence_store 读到未知围栏状态，这一行已跳过："
                       << fenceId << query.value(1).toString();
            continue;
        }
        version.name = query.value(2).toString();
        QString regionError;
        if (!regionsFromJsonText(query.value(3).toString(), &version.regions, &regionError)) {
            qWarning() << "pad_fence_store" << fenceId << "v" << version.version
                       << "的 regions 读不出来：" << regionError;
        }
        version.plan = QJsonDocument::fromJson(query.value(4).toString().toUtf8()).object();
        version.owner = QJsonDocument::fromJson(query.value(5).toString().toUtf8()).object();
        version.framePath = query.value(6).toString();
        version.appliedAt = query.value(7).toString();
        version.createdAt = query.value(8).toString();
        version.updatedAt = query.value(9).toString();
        out.append(version);
    }
    return out;
}

QList<PadFence> PadFenceStore::fencesForTask(const QString &taskId) const
{
    QList<PadFence> out;
    QSqlDatabase db = storeDb("fencesForTask");
    if (!db.isOpen())
        return out;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT fence_id, task_id, camera_id, stream_id FROM pad_fences "
        "WHERE task_id = :task_id ORDER BY fence_id"));
    query.bindValue(QStringLiteral(":task_id"), taskId);
    if (!query.exec()) {
        qWarning() << "pad_fence_store fencesForTask failed:" << query.lastError().text();
        return out;
    }
    while (query.next()) {
        PadFence fence;
        fence.fenceId = query.value(0).toString();
        fence.taskId = query.value(1).toString();
        fence.cameraId = query.value(2).toString();
        fence.streamId = query.value(3).toString();
        fence.versions = loadVersions(fence.fenceId);
        out.append(fence);
    }
    return out;
}

bool PadFenceStore::loadFence(const QString &fenceId, PadFence *out) const
{
    QSqlDatabase db = storeDb("loadFence");
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT fence_id, task_id, camera_id, stream_id FROM pad_fences WHERE fence_id = :id"));
    query.bindValue(QStringLiteral(":id"), fenceId);
    if (!query.exec()) {
        qWarning() << "pad_fence_store loadFence failed:" << query.lastError().text();
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

bool PadFenceStore::loadFenceByTaskCamera(const QString &taskId, const QString &cameraId,
                                          PadFence *out) const
{
    QSqlDatabase db = storeDb("loadFenceByTaskCamera");
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT fence_id, task_id, camera_id, stream_id FROM pad_fences "
        "WHERE task_id = :task_id AND camera_id = :camera_id"));
    query.bindValue(QStringLiteral(":task_id"), taskId);
    query.bindValue(QStringLiteral(":camera_id"), cameraId);
    if (!query.exec()) {
        qWarning() << "pad_fence_store loadFenceByTaskCamera failed:" << query.lastError().text();
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

int PadFenceStore::fenceCount() const
{
    QSqlDatabase db = storeDb("fenceCount");
    if (!db.isOpen())
        return 0;
    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM pad_fences")) || !query.next())
        return 0;
    return query.value(0).toInt();
}

QString PadFenceStore::nextFenceId() const
{
    // 扫一遍现存的 fence_id 取尾号最大值 +1。
    // 为什么不用 rowid / AUTOINCREMENT：pad_fences 的主键是 TEXT（那个号要出现在
    // URL 与 Pad 的本地键里，得是人能读的样子），而 SQLite 的 rowid 在被删过之后会复用，
    // 复用就违反 §13.4.1 那句"fenceId 必须稳定"。
    int maxSeq = kFirstFenceSeq - 1;
    QSqlDatabase db = storeDb("nextFenceId");
    if (db.isOpen()) {
        QSqlQuery query(db);
        if (query.exec(QStringLiteral("SELECT fence_id FROM pad_fences"))) {
            while (query.next()) {
                const QString id = query.value(0).toString();
                const int dash = id.lastIndexOf(QLatin1Char('-'));
                if (dash < 0 || dash == id.size() - 1)
                    continue;
                bool ok = false;
                const int seq = id.mid(dash + 1).toInt(&ok);
                if (ok && seq > maxSeq)
                    maxSeq = seq;
            }
        }
    }
    return QStringLiteral("FENCE-%1").arg(maxSeq + 1, 3, 10, QLatin1Char('0'));
}

// ============================================================================
// save-draft（§5.3 那张状态组合表全在这里）
// ============================================================================

namespace {

/// 往 pad_fence_versions 插一条 DRAFT 行。created/updated 同一时刻（新建就是这一刻）。
bool insertDraftRow(const QString &fenceId, int version, const PadFenceStore::DraftInput &input,
                    QSqlDatabase &db, QString *detail)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT INTO pad_fence_versions "
        "(fence_id, version, status, name, regions_json, plan_json, owner_json, "
        " frame_path, applied_at, created_at, updated_at) "
        "VALUES (:fence_id, :version, 'DRAFT', :name, :regions, :plan, :owner, "
        " NULL, NULL, :now, :now)"));
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    query.bindValue(QStringLiteral(":version"), version);
    query.bindValue(QStringLiteral(":name"), input.name);
    query.bindValue(QStringLiteral(":regions"), regionsToJsonText(input.regions));
    query.bindValue(QStringLiteral(":plan"),
                    QString::fromUtf8(QJsonDocument(input.plan).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":owner"),
                    QString::fromUtf8(QJsonDocument(input.owner).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":now"), internalNow());
    if (!query.exec()) {
        if (detail)
            *detail = QStringLiteral("INSERT pad_fence_versions: %1").arg(query.lastError().text());
        return false;
    }
    query.finish();   // 留着活动语句会让同连接上的下一条 UPDATE/COMMIT 失败（实测踩过）
    return true;
}

/// 版本行里最大的那个号（复制/新建按 max+1，§5.2）
int maxVersionOf(const PadFence &fence)
{
    int max = 0;
    for (const PadFenceVersion &one : fence.versions)
        max = qMax(max, one.version);
    return max;
}

/// pad_fences.updated_at 推前。单独一个函数是因为四个写分支都要做同一件事，
/// 而"漏推这条"的表现是 GET /fences 里围栏级时间不动——Pad 不报错，只是看着不对。
bool touchFenceUpdated(QSqlDatabase &db, const QString &fenceId, const QString &when,
                       QString *detail)
{
    QSqlQuery query(db);
    query.prepare(QStringLiteral("UPDATE pad_fences SET updated_at = :now WHERE fence_id = :id"));
    query.bindValue(QStringLiteral(":now"), when);
    query.bindValue(QStringLiteral(":id"), fenceId);
    if (!query.exec()) {
        if (detail)
            *detail = QStringLiteral("UPDATE pad_fences.updated_at: %1").arg(query.lastError().text());
        return false;
    }
    query.finish();
    return true;
}

}  // namespace

PadFenceStore::DraftOutcome PadFenceStore::saveDraft(const DraftInput &input,
                                                     QString *fenceIdOut, int *versionOut,
                                                     QString *detail) const
{
    QSqlDatabase db = storeDb("saveDraft");
    if (!db.isOpen()) {
        if (detail)
            *detail = QStringLiteral("idge.db 没开");
        return DraftOutcome::StoreError;
    }
    const QString now = internalNow();

    // ── 分支一：不带 fenceId = 新建 ────────────────────────────────────
    if (input.fenceId.isEmpty()) {
        PadFence existing;
        if (loadFenceByTaskCamera(input.taskId, input.cameraId, &existing)) {
            // §5.3：已有草稿 → 2004；已有围栏但没草稿 → 2006（"该摄像头已有围栏，不能重复创建"）
            return existing.draftVersion() != 0 ? DraftOutcome::DraftAlreadyExists
                                                : DraftOutcome::CameraMismatch;
        }
        const QString fenceId = nextFenceId();
        QSqlQuery query(db);
        query.prepare(QStringLiteral(
            "INSERT INTO pad_fences (fence_id, task_id, camera_id, stream_id, created_at, updated_at) "
            "VALUES (:fence_id, :task_id, :camera_id, :stream_id, :now, :now)"));
        query.bindValue(QStringLiteral(":fence_id"), fenceId);
        query.bindValue(QStringLiteral(":task_id"), input.taskId);
        query.bindValue(QStringLiteral(":camera_id"), input.cameraId);
        query.bindValue(QStringLiteral(":stream_id"), input.streamId);
        query.bindValue(QStringLiteral(":now"), now);
        if (!query.exec()) {
            // 唯一索引 (task_id, camera_id) 撞上也会走到这里：那就是并发下另一条先插成功了，
            // 报 2006（已有围栏）比报 3001 更贴近对端该做的事。
            const QString text = query.lastError().text();
            if (detail)
                *detail = QStringLiteral("INSERT pad_fences: %1").arg(text);
            return text.contains(QStringLiteral("UNIQUE"), Qt::CaseInsensitive)
                ? DraftOutcome::CameraMismatch
                : DraftOutcome::StoreError;
        }
        query.finish();
        if (!insertDraftRow(fenceId, 1, input, db, detail))
            return DraftOutcome::StoreError;
        if (fenceIdOut)
            *fenceIdOut = fenceId;
        if (versionOut)
            *versionOut = 1;
        return DraftOutcome::Created;
    }

    // ── 分支二：带 fenceId = 编辑草稿 / 复制版本 ────────────────────────
    PadFence fence;
    if (!loadFence(input.fenceId, &fence))
        return DraftOutcome::FenceNotFound;       // 2002
    // §5.3"cameraId 与围栏不一致：2006"。taskId 同判据：两个键一起才定位一条围栏，
    // 让一条 FENCE-001 在别的 taskId 底下被改名，等于把 §5.1 的唯一键搞没。
    if (fence.cameraId != input.cameraId || fence.taskId != input.taskId) {
        if (detail) {
            *detail = QStringLiteral("cameraId/taskId 与围栏不一致：围栏是 %1/%2，请求是 %3/%4")
                          .arg(fence.cameraId, fence.taskId, input.cameraId, input.taskId);
        }
        return DraftOutcome::CameraMismatch;       // 2006
    }
    const int draft = fence.draftVersion();

    if (input.hasSourceVersion) {
        // 复制：已有草稿 → 2004；缺/找不到来源版本 → 分别 2005 / 2002
        if (draft != 0)
            return DraftOutcome::DraftAlreadyExists;
        PadFenceVersion source;
        if (!fence.findVersion(input.sourceVersion, &source)) {
            if (detail) {
                *detail = QStringLiteral("%1 里没有版本 %2").arg(fence.fenceId)
                              .arg(input.sourceVersion);
            }
            return DraftOutcome::SourceVersionNotFound;
        }
        // ⚠ 这里有一条**规约没写死**的取舍，已记进 README 的待确认清单：
        //   复制时 regions/name/plan/owner 用的是**本次请求体**里的值，sourceVersion 只用来
        //   "证明这条新版本是从哪个已有版本派生的"。§5.3 说"新版本只能来自复制/编辑现有版本"，
        //   但 save-draft 的 body 里 regions 是必填 —— 于是"到底以谁为准"有两种读法。
        //   选请求体是因为：Pad 的界面就是"复制一份再改"，改完一起提交；
        //   以库里那份为准会让用户编辑完提交后看到的还是老形状（更坏）。
        const int nextVersion = maxVersionOf(fence) + 1;
        if (!insertDraftRow(fence.fenceId, nextVersion, input, db, detail))
            return DraftOutcome::StoreError;
        touchFenceUpdated(db, fence.fenceId, now, detail);
        if (fenceIdOut)
            *fenceIdOut = fence.fenceId;
        if (versionOut)
            *versionOut = nextVersion;
        return DraftOutcome::Copied;
    }

    if (draft == 0)
        return DraftOutcome::NotDraftToEdit;      // 2005（没有可编辑的草稿）

    // 编辑草稿：**版本号不变**、frameUrl 仍是 NULL（§5.2 明写草稿没有生效画面）
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE pad_fence_versions "
        "SET name = :name, regions_json = :regions, plan_json = :plan, owner_json = :owner, "
        "    updated_at = :now "
        "WHERE fence_id = :fence_id AND version = :version AND status = 'DRAFT'"));
    query.bindValue(QStringLiteral(":name"), input.name);
    query.bindValue(QStringLiteral(":regions"), regionsToJsonText(input.regions));
    query.bindValue(QStringLiteral(":plan"),
                    QString::fromUtf8(QJsonDocument(input.plan).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":owner"),
                    QString::fromUtf8(QJsonDocument(input.owner).toJson(QJsonDocument::Compact)));
    query.bindValue(QStringLiteral(":now"), now);
    query.bindValue(QStringLiteral(":fence_id"), fence.fenceId);
    query.bindValue(QStringLiteral(":version"), draft);
    if (!query.exec() || query.numRowsAffected() <= 0) {
        if (detail)
            *detail = QStringLiteral("UPDATE 草稿行没命中：%1").arg(query.lastError().text());
        return DraftOutcome::StoreError;
    }
    query.finish();
    // streamId 允许随草稿改（Pad 重选一路流）；cameraId/taskId 不改——那是围栏的身份
    QSqlQuery streamQuery(db);
    streamQuery.prepare(QStringLiteral(
        "UPDATE pad_fences SET stream_id = :stream_id, updated_at = :now WHERE fence_id = :id"));
    streamQuery.bindValue(QStringLiteral(":stream_id"), input.streamId);
    streamQuery.bindValue(QStringLiteral(":now"), now);
    streamQuery.bindValue(QStringLiteral(":id"), fence.fenceId);
    if (!streamQuery.exec())
        qWarning() << "pad_fence_store 草稿更新后 stream_id 没写进去:" << streamQuery.lastError().text();
    streamQuery.finish();
    if (fenceIdOut)
        *fenceIdOut = fence.fenceId;
    if (versionOut)
        *versionOut = draft;
    return DraftOutcome::Edited;
}

// ============================================================================
// apply / disable / delete-draft（§5.3 后三条写接口）
// ============================================================================

PadFenceStore::ApplyOutcome PadFenceStore::applyDraft(const QString &fenceId, int version,
                                                     QString *appliedAtOut, QString *detail) const
{
    QSqlDatabase db = storeDb("applyDraft");
    if (!db.isOpen()) {
        if (detail)
            *detail = QStringLiteral("idge.db 没开");
        return ApplyOutcome::StoreError;
    }
    PadFence fence;
    if (!loadFence(fenceId, &fence))
        return ApplyOutcome::FenceNotFound;        // 2002
    PadFenceVersion target;
    if (!fence.findVersion(version, &target)) {
        if (detail)
            *detail = QStringLiteral("%1 里没有版本 %2").arg(fenceId).arg(version);
        return ApplyOutcome::WrongVersion;         // 2005
    }
    // §5.3："只有 DRAFT 可下发"，非草稿（含 DISABLED 这个终态）一律 2005。
    // 这一条判据必须在**库里**再判一次（WHERE status='DRAFT'），
    // 不能只信上面那次读——读与写之间 Pad 可能已经删过草稿/另一个 Pad 已经下发过。
    if (target.status != PadVersionStatus::Draft)
        return ApplyOutcome::NotADraft;

    const QString now = internalNow();
    {
        // 原 ACTIVE 自动转 DISABLED（§5.1"下发新草稿时旧生效版本自动转 DISABLED"）
        QSqlQuery demote(db);
        demote.prepare(QStringLiteral(
            "UPDATE pad_fence_versions SET status = 'DISABLED', updated_at = :now "
            "WHERE fence_id = :fence_id AND status = 'ACTIVE' AND version <> :version"));
        demote.bindValue(QStringLiteral(":now"), now);
        demote.bindValue(QStringLiteral(":fence_id"), fenceId);
        demote.bindValue(QStringLiteral(":version"), version);
        if (!demote.exec()) {
            if (detail)
                *detail = QStringLiteral("旧 ACTIVE 转 DISABLED 失败: %1").arg(demote.lastError().text());
            return ApplyOutcome::StoreError;
        }
        demote.finish();
    }
    {
        QSqlQuery promote(db);
        promote.prepare(QStringLiteral(
            "UPDATE pad_fence_versions SET status = 'ACTIVE', applied_at = :now, updated_at = :now "
            "WHERE fence_id = :fence_id AND version = :version AND status = 'DRAFT'"));
        promote.bindValue(QStringLiteral(":now"), now);
        promote.bindValue(QStringLiteral(":fence_id"), fenceId);
        promote.bindValue(QStringLiteral(":version"), version);
        if (!promote.exec()) {
            if (detail)
                *detail = QStringLiteral("版本转 ACTIVE 失败: %1").arg(promote.lastError().text());
            return ApplyOutcome::StoreError;
        }
        if (promote.numRowsAffected() <= 0) {
            promote.finish();
            // 0 行 = 上面那次读之后它已经不是草稿了。这时候**已经执行的那条降级**要退回去吗？
            // 不退：退回去需要知道原来那条 ACTIVE 的号，而它已经被覆盖成 DISABLED——
            // 那种恢复比"这一版没下发成功"更容易造成两个 ACTIVE。
            // 走到这里的真实条件几乎只有并发下发，而本层是单线程 GUI 写，
            // 留着这段是为了"哪天加了第二个写者"时不静默错下去。
            if (detail)
                *detail = QStringLiteral("下发时这一版已经不是草稿（被并发改掉了）");
            return ApplyOutcome::NotADraft;
        }
        promote.finish();
    }
    touchFenceUpdated(db, fenceId, now, detail);
    if (appliedAtOut)
        *appliedAtOut = now;
    return ApplyOutcome::Applied;
}

PadFenceStore::DisableOutcome PadFenceStore::disableVersion(const QString &fenceId, int version,
                                                            QString *disabledAtOut,
                                                            QString *detail) const
{
    QSqlDatabase db = storeDb("disableVersion");
    if (!db.isOpen()) {
        if (detail)
            *detail = QStringLiteral("idge.db 没开");
        return DisableOutcome::StoreError;
    }
    PadFence fence;
    if (!loadFence(fenceId, &fence))
        return DisableOutcome::FenceNotFound;      // 2002
    PadFenceVersion target;
    if (!fence.findVersion(version, &target)) {
        if (detail)
            *detail = QStringLiteral("%1 里没有版本 %2").arg(fenceId).arg(version);
        return DisableOutcome::WrongVersion;       // 2005
    }
    if (target.status != PadVersionStatus::Active)
        return DisableOutcome::NotActive;          // 2005（停用只作用于生效版本）
    const QString now = internalNow();
    QSqlQuery query(db);
    // DISABLED 是终态：没有"重新启用"，也不删画面（历史生效画面还要能回看）
    query.prepare(QStringLiteral(
        "UPDATE pad_fence_versions SET status = 'DISABLED', updated_at = :now "
        "WHERE fence_id = :fence_id AND version = :version AND status = 'ACTIVE'"));
    query.bindValue(QStringLiteral(":now"), now);
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    query.bindValue(QStringLiteral(":version"), version);
    if (!query.exec() || query.numRowsAffected() <= 0) {
        if (detail)
            *detail = QStringLiteral("停用没写成：%1").arg(query.lastError().text());
        return DisableOutcome::StoreError;
    }
    query.finish();
    touchFenceUpdated(db, fenceId, now, detail);
    if (disabledAtOut)
        *disabledAtOut = now;
    return DisableOutcome::Disabled;
}

PadFenceStore::DeleteOutcome PadFenceStore::deleteDraft(const QString &fenceId, int version,
                                                        QString *detail) const
{
    QSqlDatabase db = storeDb("deleteDraft");
    if (!db.isOpen()) {
        if (detail)
            *detail = QStringLiteral("idge.db 没开");
        return DeleteOutcome::StoreError;
    }
    PadFence fence;
    if (!loadFence(fenceId, &fence))
        return DeleteOutcome::FenceNotFound;       // 2002
    PadFenceVersion target;
    if (!fence.findVersion(version, &target)) {
        if (detail)
            *detail = QStringLiteral("%1 里没有版本 %2").arg(fenceId).arg(version);
        return DeleteOutcome::WrongVersion;        // 2005
    }
    if (target.status != PadVersionStatus::Draft)
        return DeleteOutcome::NotADraft;           // 2005（"只有 DRAFT 可删除"）
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "DELETE FROM pad_fence_versions WHERE fence_id = :fence_id AND version = :version "
        "AND status = 'DRAFT'"));
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    query.bindValue(QStringLiteral(":version"), version);
    if (!query.exec() || query.numRowsAffected() <= 0) {
        if (detail)
            *detail = QStringLiteral("删草稿行没命中：%1").arg(query.lastError().text());
        return DeleteOutcome::StoreError;
    }
    query.finish();
    // §5.2"删除草稿后可能复用刚删除的版本号"：所以下一次复制按 max+1 自然会把同号再发一次，
    // 这里**不**留任何"已用号"记录——那是与规约相反的行为。
    touchFenceUpdated(db, fenceId, internalNow(), detail);
    return DeleteOutcome::Deleted;
}

bool PadFenceStore::setFramePath(const QString &fenceId, int version, const QString &framePath) const
{
    QSqlDatabase db = storeDb("setFramePath");
    if (!db.isOpen())
        return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "UPDATE pad_fence_versions SET frame_path = :path, updated_at = :now "
        "WHERE fence_id = :fence_id AND version = :version"));
    query.bindValue(QStringLiteral(":path"), framePath);
    query.bindValue(QStringLiteral(":now"), internalNow());
    query.bindValue(QStringLiteral(":fence_id"), fenceId);
    query.bindValue(QStringLiteral(":version"), version);
    if (!query.exec()) {
        qWarning() << "pad_fence_store setFramePath failed:" << query.lastError().text();
        return false;
    }
    return true;
}

}  // namespace geofence
