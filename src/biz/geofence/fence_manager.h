#pragma once

// ============================================================================
// fence_manager.h - 电子围栏数据管理器
// ============================================================================
//
// 职责：
//   1. 管理各通道的围栏数据（增删改查）
//   2. 全局参数（enabled / alarmClass / throttleSeconds）从 config.json 读写
//   3. 围栏**形状数据**存 idge.db 的 native_fences / native_fence_versions（按 taskId 作用域），
//      替代此前整文件覆写 config.json 的做法
//
// 形状存储口径（2026-11-05 起）：
//   - 每条 (taskId, cameraId=CHn) 一条围栏，多版本（每次编辑建新版 ACTIVE，旧版 DISABLED 留历史）
//   - 每个形状有稳定 regionId（本机生成，形状级追踪：删一个不影响其它形状的 id）
//   - 未登录不允许画（登录态是写围栏的硬前置）
//   - 登录/登出时 reloadForTask() 切换到对应 taskId 的围栏
//
// 顶点在本类里永远是**归一化内容坐标**（见 fence_shape.h）。
//
// 线程安全：QMutex 保护成员；UI 线程写、解码线程读（channelFence 按值返回快照）。
// ============================================================================

#include "fence_shape.h"
#include <QObject>
#include <QMap>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

class ConfigManager;

namespace geofence {

class FenceManager : public QObject {
    Q_OBJECT
public:
    static FenceManager &instance();

    // ========================================================================
    // 加载
    // ========================================================================
    // 启动与改参数时调用：从 config.json 读 enabled/alarmClass，再从 DB 加载
    // 当前 taskId（未登录=空串）下的围栏形状。
    void loadFromConfig();

    // 登录/登出后切换 taskId：清空内存形状，从 DB 加载该 taskId 下的围栏。
    // taskId 为空 = 未登录，内存形状清空（检测不再用围栏，但 DB 里历史保留）。
    void reloadForTask(const QString &taskId);

    // ========================================================================
    // 围栏总开关 / 报警类别（全局参数，仍走 config.json）
    // ========================================================================
    bool enabled() const;
    void setEnabled(bool enabled);
    QString alarmClass() const;

    // ========================================================================
    // 通道围栏操作（写操作必须已登录，否则直接返回不落库）
    // ========================================================================
    ChannelFence channelFence(int channel) const;
    void setChannelFence(int channel, const ChannelFence &fence);
    void addShape(int channel, const FenceShape &shape);
    void removeShape(int channel, int index);
    void clearChannel(int channel);
    bool hasFence(int channel) const;

    // ========================================================================
    // 登录态
    // ========================================================================
    /// 是否已登录（写围栏的硬前置：未登录时 addShape/removeShape/clearChannel 拒绝落库）
    static bool isLoggedIn();

    // ========================================================================
    // 日志
    // ========================================================================
    void postLog(const QString &category, const QString &message);

signals:
    void fenceChanged(int channel);
    void logMessage(const QString &category, const QString &message);

private:
    FenceManager() = default;

    // 当前作用域的 taskId（登录态；未登录为空串）。决定从 DB 加载哪批围栏。
    QString currentTaskId_;

    mutable QMutex mutex_;
    bool enabled_ = false;
    QString alarmClass_ = "person";
    QMap<int, ChannelFence> fences_;

    // 把内存里某通道的形状写成新 ACTIVE 版本落库（调用方已持锁）。
    // 形状没有 regionId 的会就地分配一个。
    void persistChannelLocked(int channel);

    // 为新形状分配一个稳定的 regionId（随机 hex，格式 RG-<8hex>）。
    static QString generateRegionId();

    // FenceShape ↔ PadRegion 互转（坐标 0-10000 ↔ 0-1，role ↔ ruleType）。
    static QVector<class PadRegion> shapesToRegions(const std::vector<FenceShape> &shapes);
    static std::vector<FenceShape> regionsToShapes(const QVector<class PadRegion> &regions);

    // 当前登录的 taskId；未登录返回空串。
    static QString currentTaskId();
};

} // namespace geofence
