#pragma once

// ============================================================================
// fence_manager.h - 电子围栏数据管理器
// ============================================================================
//
// 职责：
//   1. 管理各通道的围栏数据（增删改查）
//   2. 从 config.json 加载围栏配置（含旧像素坐标的一次性换算）
//   3. 将围栏配置保存到 config.json
//
// 围栏顶点在本类里永远是**归一化内容坐标**（见 fence_shape.h）：overlay 只画它，
// checker 只比它，config 只存它。旧的 overlay widget 尺寸记录已整条删除
// （判定不再需要画布几何），只在加载老配置时用作换算分母。
//
// 使用方式：
//   FenceManager::instance().loadFromConfig();    // 启动时加载
//   FenceManager::instance().addShape(ch, shape); // 添加围栏
//   FenceManager::instance().saveToConfig();      // 保存到文件
//
// 线程安全：
//   通过 QMutex 保护所有成员变量的读写
//   UI 线程可调用 setEnabled/addShape/removeShape 等写操作
//   解码线程同时调用 channelFence() 读取数据（按值返回快照）
//
// ============================================================================

#include "fence_shape.h"
#include <QObject>
#include <QMap>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>

class ConfigManager;

namespace geofence {

class FenceManager : public QObject {
    Q_OBJECT
public:
    // 获取单例实例
    static FenceManager &instance();

    // ========================================================================
    // 配置加载/保存
    // ========================================================================
    // 从 config.json 读取 geofence 配置段
    void loadFromConfig();

    // 将当前围栏数据写入 config.json
    void saveToConfig();

    // ========================================================================
    // 围栏总开关
    // ========================================================================
    // 报警"模式"不再是这里的成员：禁区/岗区域是**每个形状自带的角色**
    // （FenceShape::role，判定见 FenceChecker::checkDetection），
    // 一条通道可以同时有两种角色。
    // 旧的全局 geofence.mode 只在 loadFromConfig 里被读一次，用来给没有 role 键的
    // 老形状补角色；运行期无人读它，saveToConfig 也不再按它写回（固定写退役兼容值）。
    bool enabled() const;
    void setEnabled(bool enabled);

    // ========================================================================
    // 通道围栏操作
    // ========================================================================
    // 获取指定通道的围栏配置（按值返回，线程安全）
    ChannelFence channelFence(int channel) const;

    // 设置指定通道的围栏配置（替换）
    void setChannelFence(int channel, const ChannelFence &fence);

    // 添加一个围栏形状到指定通道
    void addShape(int channel, const FenceShape &shape);

    // 删除指定通道的第 index 个围栏形状
    void removeShape(int channel, int index);

    // 清空指定通道的所有围栏
    void clearChannel(int channel);

    // 指定通道是否有围栏
    bool hasFence(int channel) const;

    // ========================================================================
    // 围栏报警类别
    // ========================================================================
    // 触发围栏报警的类别（默认 "person"）。围栏不属于级联槽位，判定只看
    // "这一路里是不是人"，所以是单值而不是列表；config 键 geofence.alarmClass。
    QString alarmClass() const;

    // ========================================================================
    // 发送日志到 UI（迁移告警等，category 固定 "fence"）
    // ========================================================================
    void postLog(const QString &category, const QString &message);

signals:
    // 围栏数据变化信号（通知 overlay 刷新）
    void fenceChanged(int channel);
    // 围栏日志信号（通知 UI 显示）
    void logMessage(const QString &category, const QString &message);

private:
    FenceManager() = default;

    mutable QMutex mutex_;                                  // 保护所有成员变量的互斥锁
    bool enabled_ = false;                                  // 围栏总开关（通道内混角色时无法只关一种）
    QString alarmClass_ = "person";                         // 触发围栏报警的类别（loadFromConfig 从 config 读，缺失回落 person）
    QMap<int, ChannelFence> fences_;                        // 各通道围栏数据（顶点为归一化坐标）

    // 仍停留在旧 widget 像素空间的通道：老配置没记 overlay 尺寸、无从换算时进来。
    // 唯一的用途是让 saveToConfig 如实写 coord:"pixel" 而不是谎报 "norm"，
    // 于是每次启动都会再提醒一次"这条围栏位置不可信、请重画"。
    QSet<int> legacyPixel_;
};

} // namespace geofence
