// ============================================================================
// fence_manager.cpp - 电子围栏数据管理器实现
// ============================================================================

#include "fence_manager.h"
#include "ConfigManager.h"
#include <QJsonArray>
#include <QTimer>
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
//           "coord": "norm",                 // 顶点是归一化内容坐标；缺这个键 = 老的 widget 像素数据
//           "overlay_w": 400,                // 退役键，只在迁移老数据时用作换算分母
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
//
// 坐标迁移（同一处、同一条落盘路径）：归一化之前的围栏存的是 overlay widget 像素，
// 判定端要按"绘制时记录的 overlay_w/h"反算 letterbox 才能比，于是换视图/全屏即失配。
// 现在两套坐标不能共存，加载时把没有 coord:"norm" 的通道换算成归一化并写标记。
// **这个换算天生是近似的**：老配置没有记录当时的视频宽高比，无从知道内容区在画布里
// 的哪一块，只能把整块旧画布当作内容区（norm = 像素 * 10000 / overlay_w 或 / overlay_h）。
// 误差来源是黑边，画布宽高比与内容越不像、偏得越多；换算后显示与判定是自洽的
// （画出来看到的，就是报警判的），所以页面上看得见偏移、重画一次即彻底干净。
// 告警必须走 UI 日志（用户得知道围栏该复核），而 loadFromConfig 在 initForm 阶段调用、
// 那时 frmMain 还没 connect logMessage ⇒ 用 singleShot(0) 推到事件循环里再发，
// 构造函数跑完时连接已就绪。qWarning 同时留一份，命令行下启动也能看到。
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

    // 坐标迁移的中文告警攒到这里，解锁后逐条 postLog（持锁发信号会让槽里的
    // FenceManager 调用撞上同一把非递归锁）。
    QStringList coordNotices;
    bool coordMigrated = false;

    fences_.clear();
    legacyPixel_.clear();

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

        // 旧画布尺寸：退役键，只在本函数里当作换算分母用一次，不再存成员
        const int ow = chObj["overlay_w"].toInt(0);
        const int oh = chObj["overlay_h"].toInt(0);

        // 坐标空间换算：没有 coord 标记的就是归一化机制之前的 widget 像素数据。
        // 近似口径与后果见函数头注释 —— 换算完一定要在页面上复核一次。
        const bool normCoord = chObj["coord"].toString() == QLatin1String("norm");
        if (!normCoord && !cf.shapes.empty()) {
            if (ow > 0 && oh > 0) {
                for (auto &shape : cf.shapes) {
                    for (auto &p : shape.points) {
                        p.x = normFromPixel(p.x, 0, ow);
                        p.y = normFromPixel(p.y, 0, oh);
                    }
                }
                coordMigrated = true;
                coordNotices.append(QStringLiteral(
                    "通道%1 围栏已从像素坐标换算为归一化坐标（按旧画布=内容区近似），"
                    "请到视频监控页复核并按需重画").arg(ch));
            } else {
                // 没有画布尺寸就没法换算，也不猜（猜出来的围栏会静默报警）。原样留着，
                // 但如实标成 pixel，于是每次启动都会再提醒一次，直到用户重画。
                legacyPixel_.insert(ch);
                coordNotices.append(QStringLiteral(
                    "通道%1 围栏是旧像素坐标且未记录画布尺寸，无法换算，位置不可信，请删除后重画")
                    .arg(ch));
            }
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

    // 推断过角色或换算过坐标就立刻写回，把"老格式"这条路一次性封死。
    // 必须先解锁：saveToConfig 自己也要拿同一把 QMutex（非递归），持锁调用即死锁。
    if (roleInferred || coordMigrated) {
        locker.unlock();
        saveToConfig();
        if (roleInferred) {
            qDebug() << "[Fence] loadFromConfig: 老配置无 role，已按 mode=" << legacyMode
                     << " 补齐并落盘";
        }
        if (coordMigrated) {
            qDebug() << "[Fence] loadFromConfig: 老配置为像素坐标，已换算为归一化并落盘";
        }
    }

    // 告警要进 UI 日志区，但调用方是 initForm 阶段——那时 frmMain 还没 connect
    // logMessage，直接 emit 等于把这条"请复核围栏"扔掉。singleShot(0) 把它推到
    // 事件循环里发：构造函数跑完、连接已建立，而围栏也已经换算好了。
    for (const QString &notice : coordNotices) {
        qWarning() << "[Fence]" << notice;
        const QString text = notice;
        QTimer::singleShot(0, this, [this, text]() { postLog("fence", text); });
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

        // 坐标空间标记：本类内存里的顶点一律是归一化内容坐标，落盘必须带上这一点，
        // 否则下一次加载又被当成老数据换算一遍（换算不幂等）。
        // 唯一写 pixel 的情况是"老数据 + 没记录画布尺寸、换算不了"，留着它才能每次
        // 启动继续提醒用户重画；写 norm 谎报就等于把不可信的围栏说成可信的。
        chObj["coord"] = legacyPixel_.contains(it.key())
                             ? QStringLiteral("pixel") : QStringLiteral("norm");
        // overlay_w/overlay_h 不再写：判定端已经不需要画布几何，换算只在加载那一次用。

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
// 调用方是 FenceOverlay，它交上来的顶点已经是归一化坐标 ⇒ 该通道不再是"换算不了的
// 老像素数据"，摘掉 pixel 标记，否则落盘的 coord 与内存里的坐标空间不一致。
void FenceManager::setChannelFence(int channel, const ChannelFence &fence)
{
    QMutexLocker locker(&mutex_);
    fences_[channel] = fence;
    legacyPixel_.remove(channel);
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
// 坐标换算：老配置里的 overlay_w/h 只在 loadFromConfig 里读一次用作分母，
// 运行期不再记录也不再读，所以这里没有任何 widget 尺寸状态。
// ============================================================================

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
