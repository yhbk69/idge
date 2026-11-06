#ifndef NATIVE_FENCE_STORE_H
#define NATIVE_FENCE_STORE_H

// ============================================================================
// native_fence_store.h — 本机围栏（驱动检测的那套）的 DB 持久化
// ============================================================================
//
// 与 pad_fence_store 的关系：
//   pad_fence_store  = Pad 接口 v1 §5 的围栏，由 Pad 增删改查、多版本（DRAFT/ACTIVE/DISABLED）、
//                      不驱动本机检测。
//   本文件           = 本机在四宫格上画的围栏，按 taskId + cameraId(=CHn) 组织、存 idge.db、
//                      **驱动 FenceChecker 检测**。
// 两套数据结构同构（复用 PadRegion / PadFenceVersion / PadFence），但本机的状态机更简单：
//   - 没有 DRAFT：画完即生效，每次保存建一条新 ACTIVE，旧 ACTIVE → DISABLED（留历史）。
//   - regionId 由本机生成（随机 hex），形状级稳定：删一个不影响其它形状的 id。
//   - plan/owner 快照留空（本机不存施工计划快照，那是 Pad 下发围栏才有的概念）。
//
// 为什么不直接复用 pad_fences 表：
//   Pad 那边有完整的 DRAFT→APPLY 状态机与"一 ACTIVE 一 DRAFT"不变量，本机写入会破坏它。
//   用独立表 native_fences / native_fence_versions，Pad 优先策略在 GET /api/fences 层做合并。
//
// 线程：与 PadFenceStore / AlarmDAO 同口径——只在 GUI 线程用，每个方法自取当前线程连接。
// ============================================================================

#include "pad_fence_store.h"   // PadRegion / PadFenceVersion / PadFence / regionsToJsonText 等

#include <QList>
#include <QString>
#include <QVector>

namespace geofence {

class NativeFenceStore
{
public:
    /// 取某 task 下所有本机围栏（每条带全部版本，版本倒序）；无围栏返回空表
    QList<PadFence> fencesForTask(const QString &taskId) const;

    /// 取某 (taskId, cameraId) 的 ACTIVE 围栏；没有返回 false（调用方按需新建）
    bool loadActiveFence(const QString &taskId, const QString &cameraId, PadFence *out) const;

    /// 保存一个新版本为 ACTIVE：
    ///   - 若该 (taskId, cameraId) 无围栏：新建主行 + version=1 ACTIVE
    ///   - 若已有：旧 ACTIVE → DISABLED，新建 version=max+1 ACTIVE
    /// regions 里每个 region 的 regionId 由调用方（FenceManager）保证已分配且稳定。
    /// 返回新建的 fenceId；失败返回空串。
    QString saveActiveVersion(const QString &taskId, const QString &cameraId,
                              const QString &streamId, const QString &name,
                              const QVector<PadRegion> &regions) const;

    /// 库里本机围栏总数（诊断用）
    int fenceCount() const;

private:
    /// 分配下一个 fenceId：NFENCE-<自增号>，永不复用
    QString nextFenceId() const;
    /// 读一条围栏的全部版本行（倒序）
    QVector<PadFenceVersion> loadVersions(const QString &fenceId) const;
    /// 按 (taskId, cameraId) 查主行；不存在返回 false
    bool loadFenceByTaskCamera(const QString &taskId, const QString &cameraId,
                               PadFence *out) const;
};

} // namespace geofence

#endif // NATIVE_FENCE_STORE_H
