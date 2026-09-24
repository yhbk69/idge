// ============================================================================
// fence_manager.cpp - 电子围栏数据管理器实现
// ============================================================================

#include "fence_manager.h"
#include "ConfigManager.h"
#include <QJsonArray>

namespace geofence {

// ============================================================================
// 获取单例实例（线程安全，C++11 保证静态局部变量初始化线程安全）
// ============================================================================
FenceManager &FenceManager::instance()
{
    static FenceManager mgr;
    return mgr;
}

// ============================================================================
// 从 config.json 加载围栏配置
// ============================================================================
// config.json 中的 geofence 段格式：
//   {
//     "geofence": {
//       "enabled": true,
//       "mode": "inside_alarm",
//       "channels": {
//         "0": {
//           "enabled": true,
//           "overlay_w": 400,
//           "overlay_h": 300,
//           "shapes": [
//             { "type": "rectangle", "points": [[100,50],[300,200]] },
//             { "type": "polygon", "points": [[10,10],[200,10],[100,150]] }
//           ]
//         }
//       }
//     }
//   }
// ============================================================================
void FenceManager::loadFromConfig()
{
    QMutexLocker locker(&mutex_);
    ConfigManager &cfg = ConfigManager::instance();
    enabled_ = cfg.geofenceEnabled();
    mode_ = cfg.geofenceMode();
    alarmClasses_ = cfg.geofenceAlarmClasses();

    fences_.clear();
    overlaySizes_.clear();

    QJsonObject channels = cfg.geofenceChannels();
    for (auto it = channels.begin(); it != channels.end(); ++it) {
        int ch = it.key().toInt();
        QJsonObject chObj = it.value().toObject();
        ChannelFence cf;
        cf.enabled = chObj["enabled"].toBool(true);

        // 解析围栏形状数组
        QJsonArray shapesArr = chObj["shapes"].toArray();
        for (const auto &s : shapesArr) {
            QJsonObject sObj = s.toObject();
            FenceShape shape;
            QString type = sObj["type"].toString("rectangle");
            shape.type = (type == "polygon") ? ShapeType::Polygon : ShapeType::Rectangle;

            // 解析顶点坐标数组 [[x,y], [x,y], ...]
            QJsonArray pts = sObj["points"].toArray();
            for (const auto &p : pts) {
                QJsonArray pt = p.toArray();
                if (pt.size() >= 2) {
                    shape.points.push_back(Point(pt[0].toInt(), pt[1].toInt()));
                }
            }
            if (!shape.points.empty()) {
                cf.shapes.push_back(shape);
            }
        }

        // 加载 overlay 尺寸（用于坐标映射）
        int ow = chObj["overlay_w"].toInt(0);
        int oh = chObj["overlay_h"].toInt(0);
        if (ow > 0 && oh > 0) {
            overlaySizes_[ch] = qMakePair(ow, oh);
        }

        fences_[ch] = cf;
    }

    // 如果有围栏数据，自动启用
    if (!fences_.empty()) {
        for (auto it = fences_.begin(); it != fences_.end(); ++it) {
            if (!it.value().shapes.empty()) {
                enabled_ = true;
                break;
            }
        }
    }

    qDebug() << "[Fence] loadFromConfig: enabled=" << enabled_
             << "channels=" << fences_.size();
}

// ============================================================================
// 将围栏配置保存到 config.json
// ============================================================================
void FenceManager::saveToConfig()
{
    QMutexLocker locker(&mutex_);
    QJsonObject channels;
    for (auto it = fences_.begin(); it != fences_.end(); ++it) {
        QJsonObject chObj;
        chObj["enabled"] = it.value().enabled;

        // 序列化围栏形状
        QJsonArray shapesArr;
        for (const auto &shape : it.value().shapes) {
            QJsonObject sObj;
            sObj["type"] = (shape.type == ShapeType::Polygon) ? "polygon" : "rectangle";

            QJsonArray pts;
            for (const auto &p : shape.points) {
                pts.append(QJsonArray{p.x, p.y});
            }
            sObj["points"] = pts;
            shapesArr.append(sObj);
        }
        chObj["shapes"] = shapesArr;

        // 保存 overlay 尺寸
        if (overlaySizes_.contains(it.key())) {
            auto sz = overlaySizes_[it.key()];
            chObj["overlay_w"] = sz.first;
            chObj["overlay_h"] = sz.second;
        }

        channels[QString::number(it.key())] = chObj;
    }

    // 写入 ConfigManager 并保存文件
    ConfigManager::instance().setGeofenceEnabled(enabled_);
    ConfigManager::instance().setGeofenceMode(mode_);
    ConfigManager::instance().setGeofenceAlarmClasses(alarmClasses_);
    ConfigManager::instance().setGeofenceChannels(channels);
    ConfigManager::instance().save();
}

// ============================================================================
// 围栏开关和模式
// ============================================================================
bool FenceManager::enabled() const { QMutexLocker locker(&mutex_); return enabled_; }

void FenceManager::setEnabled(bool enabled) { QMutexLocker locker(&mutex_); enabled_ = enabled; }

QString FenceManager::mode() const { QMutexLocker locker(&mutex_); return mode_; }

void FenceManager::setMode(const QString &mode) { QMutexLocker locker(&mutex_); mode_ = mode; }

// ============================================================================
// 围栏报警类别
// ============================================================================
QStringList FenceManager::alarmClasses() const { QMutexLocker locker(&mutex_); return alarmClasses_; }

void FenceManager::setAlarmClasses(const QStringList &classes)
{
    QMutexLocker locker(&mutex_);
    alarmClasses_ = classes;
    if (alarmClasses_.isEmpty()) alarmClasses_.append("person");
}

// ============================================================================
// 通道围栏操作
// ============================================================================

// 获取指定通道的围栏配置（不存在则返回空配置）
// 按值返回：解码线程拿到的是数据快照，不与主线程后续写操作共享同一对象引用，
// 降低 fenceChanged/saveToConfig 与检测读取之间的竞态面（无锁但取副本）。
ChannelFence FenceManager::channelFence(int channel) const
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    return (it != fences_.end()) ? it.value() : ChannelFence();
}

// 设置指定通道的围栏配置（替换整个通道的围栏）
void FenceManager::setChannelFence(int channel, const ChannelFence &fence)
{
    QMutexLocker locker(&mutex_);
    fences_[channel] = fence;
    locker.unlock();
    emit fenceChanged(channel);
}

// 添加一个围栏形状到指定通道
void FenceManager::addShape(int channel, const FenceShape &shape)
{
    QMutexLocker locker(&mutex_);
    fences_[channel].shapes.push_back(shape);
    fences_[channel].enabled = true;
    enabled_ = true;  // 有围栏时自动启用全局开关
    locker.unlock();
    emit fenceChanged(channel);
}

// 删除指定通道的第 index 个围栏形状
void FenceManager::removeShape(int channel, int index)
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    if (it != fences_.end() && index >= 0 && index < static_cast<int>(it.value().shapes.size())) {
        it.value().shapes.erase(it.value().shapes.begin() + index);
        locker.unlock();
        emit fenceChanged(channel);
    }
}

// 清空指定通道的所有围栏
void FenceManager::clearChannel(int channel)
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    if (it != fences_.end()) {
        it.value().shapes.clear();
        locker.unlock();
        emit fenceChanged(channel);
    }
}

// 指定通道是否有围栏
bool FenceManager::hasFence(int channel) const
{
    QMutexLocker locker(&mutex_);
    auto it = fences_.find(channel);
    return it != fences_.end() && !it.value().shapes.empty();
}

// ============================================================================
// overlay 尺寸管理
// ============================================================================
// 用户绘制围栏时，顶点坐标记录的是 overlay widget 的像素值（原样保存，不归一化）。
// 检测时反向处理：把检测框脚点从视频帧空间按 letterbox 比例换算到 widget 空间，
// 再与围栏比较；换算比例依赖"绘制时"的 widget 尺寸，故必须按通道记录该尺寸。
// 若某通道未记录（overlaySize 返回 0,0），fence_checker 会退化为直接用帧坐标比较，
// 属精度下降的兜底路径，不应作为常态。
// ============================================================================

/**
 * @brief 设置指定通道的 overlay widget 尺寸
 *
 * @param channel: 通道号（0-3）
 * @param w: overlay widget 宽度（像素）
 * @param h: overlay widget 高度（像素）
 */
void FenceManager::setOverlaySize(int channel, int w, int h)
{
    QMutexLocker locker(&mutex_);
    overlaySizes_[channel] = qMakePair(w, h);
}

/**
 * @brief 获取指定通道的 overlay widget 尺寸
 *
 * @param channel: 通道号（0-3）
 * @param w: 输出的宽度（像素），无数据时返回 0
 * @param h: 输出的高度（像素），无数据时返回 0
 */
void FenceManager::overlaySize(int channel, int &w, int &h) const
{
    QMutexLocker locker(&mutex_);
    auto it = overlaySizes_.find(channel);
    if (it != overlaySizes_.end()) {
        w = it.value().first;
        h = it.value().second;
    } else {
        w = 0;
        h = 0;
    }
}

/**
 * @brief 发送围栏日志到 UI（通过信号槽跨线程安全传递）
 *
 * @param category: 日志分类（如 "fence"、"detect"、"error"）
 * @param message:  日志消息内容
 */
void FenceManager::postLog(const QString &category, const QString &message)
{
    emit logMessage(category, message);
}

} // namespace geofence
