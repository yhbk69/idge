#pragma once

// ============================================================================
// fence_manager.h - 电子围栏数据管理器
// ============================================================================
//
// 职责：
//   1. 管理各通道的围栏数据（增删改查）
//   2. 从 config.json 加载围栏配置
//   3. 将围栏配置保存到 config.json
//   4. 记录各通道的 overlay widget 尺寸（用于坐标映射）
//
// 使用方式：
//   FenceManager::instance().loadFromConfig();    // 启动时加载
//   FenceManager::instance().addShape(ch, shape); // 添加围栏
//   FenceManager::instance().saveToConfig();      // 保存到文件
//
// 线程安全：
//   通过 QMutex 保护所有成员变量的读写
//   UI 线程可调用 setEnabled/setMode/addShape 等写操作
//   解码线程同时调用 checkDetection 读取数据
//
// ============================================================================

#include "fence_shape.h"
#include <QObject>
#include <QMap>
#include <QPair>
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
    // 围栏开关和模式
    // ========================================================================
    // enabled: 是否启用围栏检测
    // mode:    "inside_alarm"  = 目标在围栏内报警（默认）
    //          "outside_alarm" = 目标在围栏外报警
    bool enabled() const;
    void setEnabled(bool enabled);

    QString mode() const;
    void setMode(const QString &mode);

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
    // 哪些检测类别会触发围栏报警（默认 "person"）
    QStringList alarmClasses() const;
    void setAlarmClasses(const QStringList &classes);

    // ========================================================================
    // overlay 尺寸管理（用于坐标映射）
    // ========================================================================
    // 围栏坐标是在 overlay widget 像素空间中绘制的，并被原样保存（不做归一化）。
    // 检测时反向映射：把检测框脚点从视频帧空间换算到同一 widget 空间再比较，
    // 这需要知道"当初绘制围栏时"的 widget 尺寸，故在此按通道记录 overlay 尺寸。
    // 尺寸随窗口缩放而变，saveFences() 会带当前 width()/height() 一并落盘。
    void setOverlaySize(int channel, int w, int h);
    void overlaySize(int channel, int &w, int &h) const;

    // 发送围栏日志到 UI
    void postLog(const QString &category, const QString &message);

signals:
    // 围栏数据变化信号（通知 overlay 刷新）
    void fenceChanged(int channel);
    // 围栏日志信号（通知 UI 显示）
    void logMessage(const QString &category, const QString &message);

private:
    FenceManager() = default;

    mutable QMutex mutex_;                                  // 保护所有成员变量的互斥锁
    bool enabled_ = false;                                  // 围栏总开关
    QString mode_ = "inside_alarm";                         // 报警模式
    QStringList alarmClasses_ = {"person"};                 // 触发围栏报警的类别
    QMap<int, ChannelFence> fences_;                        // 各通道围栏数据
    QMap<int, QPair<int,int>> overlaySizes_;                // 各通道 overlay 尺寸
};

} // namespace geofence
