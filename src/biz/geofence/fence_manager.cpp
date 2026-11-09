// ============================================================================
// fence_manager.cpp - 电子围栏数据管理器实现
// ============================================================================
//
// 形状数据现在存 idge.db（native_fences / native_fence_versions），按 taskId 作用域。
// config.json 只保留全局参数（enabled / alarmClass / throttleSeconds）。
//
// 一次性迁移：启动时若 native_fences 表空但 config.json 的 geofence.channels 有形状，
// 把那些形状灌进 DB（taskId="" 作为全局历史），然后清掉 config.json 里的 shapes，
// 此后形状只走 DB。老像素坐标换算、role 推断仍在迁移路径里跑一次。
// ============================================================================

#include "fence_manager.h"
#include "native_fence_store.h"
#include "ConfigManager.h"
#include "../session/construction_session.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTimer>
#include <QDebug>
#include <QRandomGenerator>

namespace geofence {

FenceManager &FenceManager::instance()
{
    static FenceManager mgr;
    return mgr;
}

// ============================================================================
// 登录态 / taskId
// ============================================================================
bool FenceManager::isLoggedIn()
{
    return session::ConstructionSession::instance().isLoggedIn();
}

QString FenceManager::currentTaskId()
{
    if (!isLoggedIn())
        return QString();
    return session::ConstructionSession::instance().snapshot().taskId;
}

// ============================================================================
// regionId 生成：随机 8 位 hex，前缀 RG-。碰撞概率可忽略，且形状级稳定。
// ============================================================================
QString FenceManager::generateRegionId()
{
    const quint32 r = QRandomGenerator::global()->generate();
    return QStringLiteral("RG-%1").arg(r, 8, 16, QLatin1Char('0'));
}

// ============================================================================
// FenceShape ↔ PadRegion
// ============================================================================
// 坐标：FenceShape 用 0-10000，PadRegion 用 0-1；role forbidden→INTRUSION，workzone→CROSS。
// 矩形：FenceShape 存 2 个对角点，PadRegion RECT 要 4 个顶点（左上→右上→右下→左下）。
// ============================================================================
QVector<PadRegion> FenceManager::shapesToRegions(const std::vector<FenceShape> &shapes)
{
    QVector<PadRegion> out;
    out.reserve(static_cast<int>(shapes.size()));
    for (const FenceShape &s : shapes) {
        if (s.points.empty())
            continue;
        PadRegion r;
        r.regionId = s.regionId.isEmpty() ? generateRegionId() : s.regionId;
        r.shapeType = (s.type == ShapeType::Polygon) ? QStringLiteral("POLYGON")
                                                     : QStringLiteral("RECT");
        r.ruleType = (s.role == FenceRole::Workzone) ? QStringLiteral("CROSS")
                                                     : QStringLiteral("INTRUSION");
        if (s.type == ShapeType::Rectangle && s.points.size() >= 2) {
            // 两个对角点 → 四个顶点
            const Rectangle rect = s.toRect();
            r.points = {QPointF(rect.left, rect.top), QPointF(rect.right, rect.top),
                        QPointF(rect.right, rect.bottom), QPointF(rect.left, rect.bottom)};
        } else {
            for (const Point &p : s.points)
                r.points.append(QPointF(p.x, p.y));
        }
        // 0-10000 → 0-1
        for (QPointF &p : r.points) {
            p.setX(p.x() / static_cast<double>(kFenceNormBase));
            p.setY(p.y() / static_cast<double>(kFenceNormBase));
        }
        out.append(r);
    }
    return out;
}

std::vector<FenceShape> FenceManager::regionsToShapes(const QVector<PadRegion> &regions)
{
    std::vector<FenceShape> out;
    out.reserve(regions.size());
    for (const PadRegion &r : regions) {
        FenceShape s;
        s.regionId = r.regionId;
        s.type = (r.shapeType.compare(QLatin1String("POLYGON"), Qt::CaseInsensitive) == 0)
                     ? ShapeType::Polygon : ShapeType::Rectangle;
        s.role = (r.ruleType.compare(QLatin1String("CROSS"), Qt::CaseInsensitive) == 0)
                     ? FenceRole::Workzone : FenceRole::Forbidden;
        for (const QPointF &p : r.points) {
            s.points.push_back(Point(
                static_cast<int>(p.x() * kFenceNormBase + 0.5),
                static_cast<int>(p.y() * kFenceNormBase + 0.5)));
        }
        // RECT 存 4 点 → FenceShape 矩形只存 2 个对角点（左上、右下）
        if (s.type == ShapeType::Rectangle && s.points.size() >= 4) {
            const Rectangle rect = s.toRect();
            s.points = {Point(rect.left, rect.top), Point(rect.right, rect.bottom)};
        }
        out.push_back(s);
    }
    return out;
}

// ============================================================================
// 把内存里某通道的形状写成新 ACTIVE 版本落库（调用方已持锁）
// ============================================================================
void FenceManager::persistChannelLocked(int channel)
{
    const QString taskId = currentTaskId_.isEmpty() ? currentTaskId() : currentTaskId_;
    // 未登录不落库（登录态是写围栏的硬前置）
    if (taskId.isEmpty())
        return;

    const QString cameraId = QStringLiteral("CH%1").arg(channel);
    const QString name = QStringLiteral("通道%1围栏").arg(channel);

    auto it = fences_.find(channel);
    QVector<PadRegion> regions;
    if (it != fences_.end()) {
        // 给没 regionId 的形状分配一个（就地改内存里的副本，下次保存带得走）
        for (FenceShape &shape : it.value().shapes) {
            if (shape.regionId.isEmpty())
                shape.regionId = generateRegionId();
        }
        regions = shapesToRegions(it.value().shapes);
    }

    NativeFenceStore store;
    const QString savedId = store.saveActiveVersion(taskId, cameraId, QString(), name, regions);
    if (savedId.isEmpty()) {
        // 落库失败就**不动**内存里的 fenceId/version：留旧值的含义是"检测与报警都用
        // 上一次真正存进库的那一版"，两边一致。反过来若在这里回读，读到的确实是那一版，
        // 但内存里的形状已经是用户刚画的、库里没有的 —— 那才是会把不存在的版本号
        // 打进报警的情形。与迁移那条路径同一口径：失败就保留原状，下次重试。
        qWarning() << "[Fence] 通道" << channel
                   << "围栏落库失败，本次编辑未生效（重启/重登会回到上一版）";
        return;
    }
    // 把刚落库的身份读回这份内存快照：报警打的 fenceId/fenceVersion 就是它。
    // 多这一次查询只发生在"用户改完围栏"那一下（GUI 线程、每个编辑动作一次），
    // 而解码线程每次都只读内存快照，不碰库。
    PadFence saved;
    if (it != fences_.end() && store.loadActiveFence(taskId, cameraId, &saved)) {
        it.value().fenceId = saved.fenceId;
        it.value().version = saved.activeVersion();
    }
}

// ============================================================================
// 从 config.json 读全局参数 + 从 DB 加载当前 taskId 的围栏形状
// ============================================================================
void FenceManager::loadFromConfig()
{
    QMutexLocker locker(&mutex_);
    ConfigManager &cfg = ConfigManager::instance();
    enabled_ = cfg.geofenceEnabled();
    alarmClass_ = cfg.geofenceAlarmClass();

    fences_.clear();
    currentTaskId_ = currentTaskId();

    NativeFenceStore store;
    const bool dbEmpty = (store.fenceCount() == 0);

    // ── 一次性迁移：DB 空但 config.json 有形状 → 灌进 DB（taskId="" 作为全局历史）──
    if (dbEmpty) {
        QJsonObject channels = cfg.geofenceChannels();
        const QString legacyMode = cfg.geofenceMode();
        bool migrationOk = true;
        int migratedCount = 0;
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            const int ch = it.key().toInt();
            const QJsonObject chObj = it.value().toObject();
            const QJsonArray shapesArr = chObj["shapes"].toArray();
            if (shapesArr.isEmpty())
                continue;

            ChannelFence cf;
            cf.enabled = chObj["enabled"].toBool(true);
            const bool normCoord = chObj["coord"].toString() == QLatin1String("norm");
            const int ow = chObj["overlay_w"].toInt(0);
            const int oh = chObj["overlay_h"].toInt(0);

            for (const auto &sVal : shapesArr) {
                QJsonObject sObj = sVal.toObject();
                FenceShape shape;
                const QString type = sObj["type"].toString("rectangle");
                shape.type = (type == "polygon") ? ShapeType::Polygon : ShapeType::Rectangle;
                shape.role = sObj.contains("role")
                                 ? roleFromString(sObj["role"].toString().toStdString())
                                 : (legacyMode == "outside_alarm" ? FenceRole::Workzone
                                                                  : FenceRole::Forbidden);
                for (const auto &p : sObj["points"].toArray()) {
                    QJsonArray pt = p.toArray();
                    if (pt.size() >= 2)
                        shape.points.push_back(Point(pt[0].toInt(), pt[1].toInt()));
                }
                if (!normCoord && !shape.points.empty()) {
                    if (ow > 0 && oh > 0) {
                        for (auto &p : shape.points) {
                            p.x = normFromPixel(p.x, 0, ow);
                            p.y = normFromPixel(p.y, 0, oh);
                        }
                    }
                }
                if (!shape.points.empty())
                    cf.shapes.push_back(shape);
            }

            if (!cf.shapes.empty()) {
                // 迁移到 DB（taskId="" 作为全局历史；登录后按 task 另画）
                const QString cameraId = QStringLiteral("CH%1").arg(ch);
                const QString name = QStringLiteral("通道%1围栏").arg(ch);
                const QString fid = store.saveActiveVersion(QString(), cameraId, QString(), name,
                                                            shapesToRegions(cf.shapes));
                if (fid.isEmpty()) {
                    qWarning() << "[Fence] 通道" << ch << "围栏迁移到 DB 失败，保留 config.json 原值下次重试";
                    migrationOk = false;
                } else {
                    ++migratedCount;
                }
            }
        }

        // 只在迁移全部成功后才清 config.json 里的 shapes（保留 enabled/alarmClass/throttle）。
        // 任一通道失败就保留 config，下次启动重试——绝不丢数据。
        if (migrationOk && migratedCount > 0) {
            QJsonObject cleaned;
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                QJsonObject chObj = it.value().toObject();
                chObj.remove("shapes");
                chObj.remove("coord");
                chObj.remove("overlay_w");
                chObj.remove("overlay_h");
                cleaned[it.key()] = chObj;
            }
            cfg.setGeofenceChannels(cleaned);
            cfg.save();
            qInfo() << "[Fence] 已将 config.json 围栏形状迁移到 DB（native_fences），config 仅保留全局参数";
        }
    }

    // 从两个围栏库把内存快照拉一遍（native + Pad 下发的，Pad 优先）
    refreshFromStoresLocked();

    qDebug() << "[Fence] loadFromConfig: enabled=" << enabled_
             << "taskId=" << currentTaskId_
             << "channels=" << fences_.size();
}

// ============================================================================
// 登录/登出后切换 taskId：重载该 task 下的围栏
// ============================================================================
void FenceManager::reloadForTask(const QString &taskId)
{
    QMutexLocker locker(&mutex_);
    currentTaskId_ = taskId;
    fences_.clear();

    if (taskId.isEmpty()) {
        // 未登录：内存清空，检测不再用围栏（DB 历史保留）。
        // ⚠ 这里与启动时的 loadFromConfig 不同：没登录时启动读的是"全局那一桶"
        // （task_id=''，迁移进来的历史围栏），而**登出**是明确要停用的动作。
        qDebug() << "[Fence] reloadForTask: 未登录，清空内存围栏";
        return;
    }

    // 抬开关（config 的 geofence.enabled 可能是模板默认的 false）由
    // refreshFromStoresLocked 统一做，这里不再重复判一次。
    refreshFromStoresLocked();
    qDebug() << "[Fence] reloadForTask:" << taskId << "channels=" << fences_.size();
}

// ============================================================================
// 内存快照的两个来源：native_fences（本机手绘）+ pad_fences（Pad 下发，优先）
// ============================================================================
// 【为什么两张表要合起来喂检测】§5.3 的 apply 语义是"这一版在边缘设备上生效"
//   （ACTIVE 就叫"生效版本"），而改动前本机判定只读 native_fences：对端收到 200/ACTIVE、
//   本机一帧都不判，这种静默不一致比"没这个功能"更坏。规约没写两套件冲突时谁赢，
//   **Pad 优先覆盖同通道是本机定的**，已挂进 src/biz/pad/README.md 的待确认清单。
// 【两套 cameraId 编号，解析必须分开走】
//   native_fences：本机自己拼的 "CH"+**0-based** 通道号（CH0..CH3）。
//   pad_fences：Pad 原样存进来的 cameraId，而 §7.1 的 channelIdentity() 发给对端的是
//              "CH"+**1-based**（CH1..CH4）。同一个 "CH2" 在两张表里是两个通道，
//              所以这里按各自口径解析，绝不能共用一句 mid(2).toInt()。
// 【与 GET /fences 的差一处】那边只要该 cameraId 上有 Pad 围栏就藏掉本机那条
//   （草稿也算占位），这边只认 ACTIVE：草稿还没生效，拿它去判帧就是替对端改了主意。
// 【未登录（taskId 空）不并 Pad 的】pad_fences 每条都挂在具体 taskId 上，
//   空 taskId 查出来本来就是空表，不特判也不会错。
// ============================================================================
namespace {

/// pad_fences 的 cameraId → 本机通道号（0-based）。不匹配协议口径就返回 false。
bool channelFromPadCameraId(const QString &cameraId, int *out)
{
    static const QRegularExpression kPadCameraId(QStringLiteral("^CH([1-9][0-9]?)$"));
    const QRegularExpressionMatch m = kPadCameraId.match(cameraId.trimmed());
    if (!m.hasMatch())
        return false;
    const int oneBased = m.captured(1).toInt();
    if (oneBased < 1 || oneBased > ConfigManager::instance().channelCount())
        return false;
    *out = oneBased - 1;
    return true;
}

/// 一条围栏里当前生效的那一版；没有 ACTIVE 返回 false
bool activeVersionOf(const PadFence &fence, PadFenceVersion *out)
{
    for (const PadFenceVersion &v : fence.versions) {
        if (v.status == PadVersionStatus::Active) {
            *out = v;
            return true;
        }
    }
    return false;
}

}  // namespace

void FenceManager::refreshFromStores()
{
    QMutexLocker locker(&mutex_);
    refreshFromStoresLocked();
}

void FenceManager::refreshFromStoresLocked()
{
    fences_.clear();
    loadNativeFencesLocked();
    mergePadFencesLocked();
    // 内存里有形状就允许判定（与 loadFromConfig/reloadForTask 同一口径）：
    // config 的 geofence.enabled 可能是模板默认的 false，而围栏是刚生效的。
    if (!fences_.empty())
        enabled_ = true;
}

void FenceManager::loadNativeFencesLocked()
{
    NativeFenceStore store;
    const QList<PadFence> dbFences = store.fencesForTask(currentTaskId_);
    for (const PadFence &f : dbFences) {
        bool ok = false;
        const int ch = f.cameraId.mid(2).toInt(&ok);  // 本机口径："CH0" → 0
        if (!ok || ch < 0 || ch >= ConfigManager::instance().channelCount())
            continue;
        PadFenceVersion active;
        if (!activeVersionOf(f, &active))
            continue;
        ChannelFence cf;
        cf.enabled = true;
        // 版本戳随快照一起进内存：解码线程打 fenceVersion 时不能再查库
        cf.fenceId = f.fenceId;
        cf.version = active.version;
        cf.shapes = regionsToShapes(active.regions);
        fences_[ch] = cf;
    }
}

void FenceManager::mergePadFencesLocked()
{
    PadFenceStore store;
    const QList<PadFence> padFences = store.fencesForTask(currentTaskId_);
    for (const PadFence &f : padFences) {
        int ch = -1;
        if (!channelFromPadCameraId(f.cameraId, &ch)) {
            // 对端可以自由写 cameraId（规约只说它由 Pad 生成、本机原样保存）。
            // 对不上通道就不并——**并错通道比不并更坏**：那会在另一台画面上报警。
            qWarning() << "[Fence] Pad 围栏" << f.fenceId << "的 cameraId"
                       << f.cameraId << "对不上本机任何一路通道，这条不驱动检测";
            continue;
        }
        PadFenceVersion active;
        if (!activeVersionOf(f, &active))
            continue;   // 只有草稿/已停用的版本：不覆盖，继续用本机手绘那版
        ChannelFence cf;
        cf.enabled = true;
        cf.fenceId = f.fenceId;
        cf.version = active.version;
        cf.shapes = regionsToShapes(active.regions);
        // Pad 优先：与 GET /api/fences 一样，同一 cameraId 上 Pad 那台占位时本机那条
        // 不露出。差一处判据说明在这儿：那边连草稿也算占位，这边**只认 ACTIVE**——
        // 草稿还没生效，拿它去判帧等于替对端改了主意，报警也发不出去。
        fences_[ch] = cf;
    }
}

// ============================================================================
// 总开关
// ============================================================================
bool FenceManager::enabled() const { QMutexLocker locker(&mutex_); return enabled_; }

void FenceManager::setEnabled(bool enabled)
{
    QMutexLocker locker(&mutex_);
    enabled_ = enabled;
}

QString FenceManager::alarmClass() const { QMutexLocker locker(&mutex_); return alarmClass_; }

// ============================================================================
// 通道围栏读
// ============================================================================
ChannelFence FenceManager::channelFence(int channel) const
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    return (it != fences_.end()) ? it.value() : ChannelFence();
}

bool FenceManager::hasFence(int channel) const
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    return it != fences_.end() && !it.value().shapes.empty();
}

// ============================================================================
// 通道围栏写（必须已登录）
// ============================================================================
void FenceManager::setChannelFence(int channel, const ChannelFence &fence)
{
    if (!isLoggedIn()) {
        qWarning() << "[Fence] setChannelFence 被拒绝：未登录";
        return;
    }
    QMutexLocker locker(&mutex_);
    fences_[channel] = fence;
    persistChannelLocked(channel);
    locker.unlock();
    emit fenceChanged(channel);
}

void FenceManager::addShape(int channel, const FenceShape &shape)
{
    if (!isLoggedIn()) {
        qWarning() << "[Fence] addShape 被拒绝：未登录";
        return;
    }
    QMutexLocker locker(&mutex_);
    fences_[channel].shapes.push_back(shape);
    fences_[channel].enabled = true;
    enabled_ = true;
    persistChannelLocked(channel);
    locker.unlock();
    emit fenceChanged(channel);
}

void FenceManager::removeShape(int channel, int index)
{
    if (!isLoggedIn()) {
        qWarning() << "[Fence] removeShape 被拒绝：未登录";
        return;
    }
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    if (it != fences_.end() && index >= 0 && index < static_cast<int>(it.value().shapes.size())) {
        // 只删这一个，其它形状的 regionId 保持不变（形状级身份追踪）
        it.value().shapes.erase(it.value().shapes.begin() + index);
        persistChannelLocked(channel);
        locker.unlock();
        emit fenceChanged(channel);
    }
}

void FenceManager::clearChannel(int channel)
{
    if (!isLoggedIn()) {
        qWarning() << "[Fence] clearChannel 被拒绝：未登录";
        return;
    }
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    if (it != fences_.end()) {
        it.value().shapes.clear();
        persistChannelLocked(channel);  // 存一个空 regions 的新版本（历史保留）
        locker.unlock();
        emit fenceChanged(channel);
    }
}

// ============================================================================
// 日志
// ============================================================================
void FenceManager::postLog(const QString &category, const QString &message)
{
    emit logMessage(category, message);
}

} // namespace geofence
