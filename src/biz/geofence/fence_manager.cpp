// ============================================================================
// fence_manager.cpp - 电子围栏数据管理器实现
// ============================================================================

#include "fence_manager.h"
#include "ConfigManager.h"
#include <QJsonArray>
#include <QDebug>

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
//       "mode": "inside_alarm",              // 退役键，仅作兼容读写，见下
//       "channels": {
//         "0": {
//           "enabled": true,
//           "overlay_w": 400,
//           "overlay_h": 300,
//           "shapes": [
//             { "type": "rectangle", "role": "forbidden", "points": [[100,50],[300,200]] },
//             { "type": "polygon",   "role": "workzone",  "points": [[10,10],[200,10],[100,150]] }
//           ]
//         }
//       },
//       "alarmClass": "person",                 // 触发围栏报警的类别，缺失时默认 person
//       "throttleSeconds": 10                   // 围栏报警节流下限（主判据是"进入沿"，见 README）
//     }
//   }
//
// 老配置（没有 "role"）在这里就地补齐：以该文件当时的 mode 为准，
// outside_alarm → workzone，其余 → forbidden。补齐后立刻落盘，因此这个推断只发生一次
// —— 第二次加载时每个形状都带显式 role，用户重画出来的角色不会再被 mode 拍平。
// 迁移不放 ConfigManager::migrateSchemaV2()：那是一次性闸门、现场已经跑过、
// 而且它只动 geofence 顶层键、从不遍历 channels/*/shapes。
// ============================================================================
void FenceManager::loadFromConfig()
{
    QMutexLocker locker(&mutex_);
    ConfigManager &cfg = ConfigManager::instance();
    enabled_ = cfg.geofenceEnabled();
    // 围栏触发类别必须由配置决定，不能写死：级联槽位挂 PPE 模型时类名是
    // un_helmet/helmet/people/vest/workWear，硬编码的 "person" 一个都匹配不上。
    // V2 里它是单值（围栏判定只看"这一路里是不是人"），老配置的列表由
    // migrateSchemaV2() 折叠成一个；geofenceAlarmClass() 缺键时回落 "person"。
    alarmClass_ = cfg.geofenceAlarmClass();

    // 退役中的全局 mode：运行期不再有任何消费者（角色改为逐形状判定、由
    // FenceShape::role 承载），这里只留作"老配置里没有 role 的形状该补哪个角色"
    // 的唯一依据，所以读进局部变量、不存成员 —— 存了就会有人误以为它还能切行为。
    const QString legacyMode = cfg.geofenceMode();
    bool roleInferred = false;

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

            // 角色：有显式键就照它（roleFromString 认不出的值回落 forbidden），
            // 没有键说明是角色机制之前的老围栏，按当时的全局 mode 推断。
            if (sObj.contains("role")) {
                shape.role = roleFromString(sObj["role"].toString().toStdString());
            } else {
                shape.role = (legacyMode == "outside_alarm")
                                 ? FenceRole::Workzone : FenceRole::Forbidden;
                roleInferred = true;
            }

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

    // 推断过角色就立刻写回，把"role 缺失"这条路一次性封死。
    // 必须先解锁：saveToConfig 自己也要拿同一把 QMutex（非递归），持锁调用即死锁。
    if (roleInferred) {
        locker.unlock();
        saveToConfig();
        qDebug() << "[Fence] loadFromConfig: 老配置无 role，已按 mode=" << legacyMode
                 << " 补齐并落盘";
    }
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
            // 角色无条件写：老配置第一次加载时补齐，此后每次保存都带得走，
            // 手工编辑的配置文件也保证落盘后是自洽的。
            sObj["role"] = QString::fromStdString(roleToString(shape.role));

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
    // mode 是退役键：角色已下放到每个 shape，运行期没人读它。仍写一个固定值，
    // 是为了让老程序/老配置读到这个键时不至于拿到 null；它不再表达任何语义。
    ConfigManager::instance().setGeofenceEnabled(enabled_);
    ConfigManager::instance().setGeofenceMode(QStringLiteral("inside_alarm"));
    ConfigManager::instance().setGeofenceAlarmClass(alarmClass_);
    ConfigManager::instance().setGeofenceChannels(channels);
    ConfigManager::instance().save();
}

// ============================================================================
// 围栏总开关
// ============================================================================
// 通道级只有"开/关"两态：一条通道里混用禁区与岗区域时无法只关其中一种
// （要单独停用某条规则，就把那个形状删掉重画成别的角色）。
bool FenceManager::enabled() const { QMutexLocker locker(&mutex_); return enabled_; }

void FenceManager::setEnabled(bool enabled) { QMutexLocker locker(&mutex_); enabled_ = enabled; }

// ============================================================================
// 围栏报警类别
// ============================================================================
QString FenceManager::alarmClass() const { QMutexLocker locker(&mutex_); return alarmClass_; }

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
