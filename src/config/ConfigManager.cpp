// ============================================================================
// ConfigManager.cpp - 配置管理器实现
// ============================================================================
//
// 作用：
//   管理应用程序的配置信息，从 config.json 读取和保存配置。
//
// 配置文件结构 (config.json)：
//   {
//     "video": {
//       "channel1": "192.mp4",
//       "channel2": "192.mp4",
//       "channel3": "192.mp4",
//       "channel4": "192.mp4",
//       "notes": ["", "", "", ""]
//     },
//     "model": {
//       "path": "model/yolo11n.rknn",
//       "label": "model/coco_80_labels_list.txt",
//       "input_size": "640x640",
//       "type": "YOLO11"
//     },
//     "cascade": {
//       "models": [
//         {"path": "", "note": ""},
//         ...
//       ]
//     },
//     "detect": {
//       "conf_threshold": 0.25,
//       "nms_threshold": 0.45,
//       "class_num": 80,
//       "threads": 3
//     },
//     "alarm": {
//       "classes": ["person"]
//     }
//   }
//
// ============================================================================

#include "ConfigManager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

// ============================================================================
// 获取单例实例
// ============================================================================
// 使用静态局部变量保证线程安全的懒汉式单例
// C++11 标准保证静态局部变量的初始化是线程安全的
// ============================================================================
ConfigManager &ConfigManager::instance()
{
    static ConfigManager mgr;
    return mgr;
}

// ============================================================================
// 从 JSON 文件加载配置
// ============================================================================
// 如果文件不存在，自动创建默认配置并保存。
// 默认配置：
//   - 4路视频通道都指向 "192.mp4"
//   - 模型路径: model/yolo11n.rknn
//   - 标签路径: model/coco_80_labels_list.txt
//   - 置信度阈值: 0.25
//   - NMS阈值: 0.45
//
// 参数：
//   - path: 配置文件路径（默认 "config.json"）
//
// ============================================================================
void ConfigManager::load(const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    configPath_ = path;
    QFile file(path);

    // 文件不存在时，创建默认配置
    if (!file.exists()) {
        // 视频通道配置
        root_["video"] = QJsonObject{
            {"channel1", "192.mp4"},   // 通道 1 默认视频
            {"channel2", "192.mp4"},   // 通道 2 默认视频
            {"channel3", "192.mp4"},   // 通道 3 默认视频
            {"channel4", "192.mp4"},   // 通道 4 默认视频
            {"notes", QJsonArray{"", "", "", ""}}  // 通道备注
        };

        // 模型通用配置
        root_["model"] = QJsonObject{
            {"input_size", "640x640"},   // 模型输入尺寸
            {"type", "YOLO11"}           // 模型类型
        };

        // 所有模型统一在 cascade.models 中配置（包括主模型）
        // 每个模型独立指定路径、标签、备注
        root_["cascade"] = QJsonObject{
            {"models", QJsonArray{
                QJsonObject{{"path", "model/yolo11n.rknn"}, {"label", "model/coco_80_labels_list.txt"}, {"note", "默认模型"}},
                QJsonObject{{"path", ""}, {"label", ""}, {"note", ""}},
                QJsonObject{{"path", ""}, {"label", ""}, {"note", ""}},
                QJsonObject{{"path", ""}, {"label", ""}, {"note", ""}},
                QJsonObject{{"path", ""}, {"label", ""}, {"note", ""}}
            }}
        };

        // 检测参数配置
        root_["detect"] = QJsonObject{
            {"conf_threshold", 0.25},  // 置信度阈值
            {"nms_threshold", 0.45},   // NMS 阈值
            {"class_num", 80},         // 类别数量（COCO 数据集）
            {"threads", 3}             // 推理线程数
        };

        // 报警配置
        root_["alarm"] = QJsonObject{
            {"classes", QJsonArray{"person"}}  // 报警类别（检测到人时报警）
        };

        saveUnsafe();  // 保存默认配置
        return;
    }

    // 读取并解析 JSON 文件
    // 解析失败/文件损坏时不抛异常，而是把 root_ 置为空对象：
    // 此后所有 getter 会因取不到键而回退到内置默认值（0.25/0.45/80/3/"inside_alarm"...），
    // 保证程序仍可带默认配置启动；但注意这会静默丢弃用户原有配置。
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isNull()) {
            qWarning() << "[ConfigManager] JSON 解析失败:" << path;
            root_ = QJsonObject();
        } else {
            root_ = doc.object();
        }
        file.close();
    } else {
        qWarning() << "[ConfigManager] 文件打开失败:" << path;
    }

    // 从 root_ 更新缓存值（避免每次 getter 都反序列化 JSON）
    updateCache();
}

// ============================================================================
// 从 root_ 更新缓存值
// ============================================================================
// 将高频访问的配置值缓存到成员变量，getter 直接返回缓存值
// 避免每次调用 getter 都做 root_["xxx"].toObject()["yyy"] 的临时对象创建
// ============================================================================
void ConfigManager::updateCache()
{
    QJsonObject detect = root_["detect"].toObject();
    confThreshold_ = detect["conf_threshold"].toDouble(0.25);
    nmsThreshold_ = detect["nms_threshold"].toDouble(0.45);
    classNum_ = detect["class_num"].toInt(80);
    threads_ = detect["threads"].toInt(3);

    QJsonObject db = root_["database"].toObject();
    storeDetections_ = db["storeDetections"].toBool(true);
    detectionRetentionDays_ = db["detectionRetentionDays"].toInt(30);
}

// ============================================================================
// 将当前配置保存到 JSON 文件（内部不加锁版本）
// ============================================================================
void ConfigManager::saveUnsafe()
{
    QFile file(configPath_);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(root_);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

// ============================================================================
// 将当前配置保存到 JSON 文件
// ============================================================================
// 使用 Indented 格式写入，便于人工阅读和编辑
// ============================================================================
void ConfigManager::save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    saveUnsafe();
}

// ==========================================
// 视频通道配置
// ==========================================

// ============================================================================
// 获取指定通道的视频路径
// ============================================================================
// 参数：
//   - ch: 通道号，取值 1-4
//
// 返回：视频文件路径或 RTSP 地址
//       通道不存在时返回空字符串
//
// ============================================================================
QString ConfigManager::videoChannel(int ch) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject video = root_["video"].toObject();
    return video.value(QString("channel%1").arg(ch)).toString();
}

// ============================================================================
// 设置指定通道的视频路径
// ============================================================================
// 参数：
//   - ch: 通道号，取值 1-4
//   - path: 视频文件路径或 RTSP 地址
//
// ============================================================================
void ConfigManager::setVideoChannel(int ch, const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject video = root_["video"].toObject();
    video[QString("channel%1").arg(ch)] = path;
    root_["video"] = video;
}

// ==========================================
// 视频通道备注
// ==========================================

// 获取指定通道的备注
QString ConfigManager::channelNote(int ch) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject video = root_["video"].toObject();
    QJsonArray notes = video["notes"].toArray();
    if (ch >= 1 && ch <= notes.size()) {
        return notes[ch - 1].toString();
    }
    return QString();
}

// 设置指定通道的备注
void ConfigManager::setChannelNote(int ch, const QString &note)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject video = root_["video"].toObject();
    QJsonArray notes = video["notes"].toArray();
    // 补齐到 4 个
    while (notes.size() < 4) notes.append("");
    if (ch >= 1 && ch <= notes.size()) {
        notes[ch - 1] = note;
    }
    video["notes"] = notes;
    root_["video"] = video;
}

// ==========================================
// 级联模型配置
// ==========================================
// config.json 里存成 5 个对象数组：
//   "cascade": { "models": [ {"path":"..","note":".."}, ... ] }  长度5
//
// 级联模型机制：
//   - 模型 1（主模型）：由 "model.path" 配置
//   - 模型 2-6（可选）：由 "cascade.models" 配置
//   - 每个模型独立推理，结果合并后画框
// ==========================================

// 辅助函数：获取级联模型配置对象
static QJsonObject cascadeModelObject(const QJsonObject &cascade, int idx)
{
    // idx: 1~5，取第 idx 个对象，越界返回空对象
    QJsonArray arr = cascade["models"].toArray();
    if (idx >= 1 && idx <= arr.size()) {
        return arr[idx - 1].toObject();
    }
    return QJsonObject();
}

// 获取级联模型路径
QString ConfigManager::cascadeModelPath(int idx) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["path"].toString();
}

// 设置级联模型路径
void ConfigManager::setCascadeModelPath(int idx, const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    QJsonArray arr = cascade["models"].toArray();
    // 补齐到 5 个
    while (arr.size() < 5) arr.append(QJsonObject());
    if (idx >= 1 && idx <= 5) {
        QJsonObject o = arr[idx - 1].toObject();
        o["path"] = path;
        arr[idx - 1] = o;
    }
    cascade["models"] = arr;
    root_["cascade"] = cascade;
}

// 获取级联模型备注
QString ConfigManager::cascadeModelNote(int idx) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["note"].toString();
}

// 设置级联模型备注
void ConfigManager::setCascadeModelNote(int idx, const QString &note)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    QJsonArray arr = cascade["models"].toArray();
    // 补齐到 5 个
    while (arr.size() < 5) arr.append(QJsonObject());
    if (idx >= 1 && idx <= 5) {
        QJsonObject o = arr[idx - 1].toObject();
        o["note"] = note;
        arr[idx - 1] = o;
    }
    cascade["models"] = arr;
    root_["cascade"] = cascade;
}

// 获取级联模型标签文件路径
QString ConfigManager::cascadeModelLabel(int idx) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["label"].toString();
}

// 设置级联模型标签文件路径
void ConfigManager::setCascadeModelLabel(int idx, const QString &label)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject cascade = root_["cascade"].toObject();
    QJsonArray arr = cascade["models"].toArray();
    // 补齐到 5 个
    while (arr.size() < 5) arr.append(QJsonObject());
    if (idx >= 1 && idx <= 5) {
        QJsonObject o = arr[idx - 1].toObject();
        o["label"] = label;
        arr[idx - 1] = o;
    }
    cascade["models"] = arr;
    root_["cascade"] = cascade;
}

// ==========================================
// 模型配置
// ==========================================

// 获取 RKNN 模型文件路径
QString ConfigManager::modelPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["model"].toObject()["path"].toString();
}

// 设置 RKNN 模型文件路径
void ConfigManager::setModelPath(const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject model = root_["model"].toObject();
    model["path"] = path;
    root_["model"] = model;
}

// 获取标签文件路径
QString ConfigManager::labelPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["model"].toObject()["label"].toString();
}

// 设置标签文件路径
void ConfigManager::setLabelPath(const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject model = root_["model"].toObject();
    model["label"] = path;
    root_["model"] = model;
}

// 获取模型输入尺寸（格式如 "640x640"）
QString ConfigManager::inputSize() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["model"].toObject()["input_size"].toString();
}

// 获取模型类型名称（如 "YOLO11"）
QString ConfigManager::modelType() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["model"].toObject()["type"].toString();
}

// ==========================================
// 检测参数配置
// ==========================================

// ============================================================================
// 获取置信度阈值
// ============================================================================
// 阈值范围 0.0 ~ 1.0，默认 0.25
// 只有置信度高于此阈值的检测结果才会被输出
//
// 调低：检测更多目标，但误检增多
// 调高：检测更少目标，但漏检增多
// ============================================================================
double ConfigManager::confThreshold() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return confThreshold_;  // 返回缓存值，避免每次反序列化 JSON
}

// 设置置信度阈值
void ConfigManager::setConfThreshold(double val)
{
    std::lock_guard<std::mutex> lock(mutex_);
    val = qBound(0.0, val, 1.0);  // 限制范围 0.0 ~ 1.0
    QJsonObject detect = root_["detect"].toObject();
    detect["conf_threshold"] = val;
    root_["detect"] = detect;
    confThreshold_ = val;  // 更新缓存
}

// ============================================================================
// 获取 NMS（非极大值抑制）阈值
// ============================================================================
// 阈值范围 0.0 ~ 1.0，默认 0.45
// 用于消除重叠的检测框
//
// NMS 过程：
//   1. 按置信度排序所有检测框
//   2. 选择置信度最高的框
//   3. 删除与其 IoU（交并比）大于阈值的其他框
//   4. 重复直到所有框都被处理
//
// 调低：更激进地合并重叠框
// 调高：保留更多重叠框
// ============================================================================
double ConfigManager::nmsThreshold() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nmsThreshold_;  // 返回缓存值
}

// 设置 NMS 阈值
void ConfigManager::setNmsThreshold(double val)
{
    std::lock_guard<std::mutex> lock(mutex_);
    val = qBound(0.0, val, 1.0);  // 限制范围 0.0 ~ 1.0
    QJsonObject detect = root_["detect"].toObject();
    detect["nms_threshold"] = val;
    root_["detect"] = detect;
    nmsThreshold_ = val;  // 更新缓存
}

// 获取检测类别数量（默认 80，COCO 数据集）
int ConfigManager::classNum() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return classNum_;  // 返回缓存值
}

// ============================================================================
// 获取推理线程数
// ============================================================================
// 默认 3，对应 RK3588 的 3 个 NPU 核心
// 每个线程绑定一个 NPU 核心，可以并行推理
// ============================================================================
int ConfigManager::threads() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_;  // 返回缓存值
}

// ==========================================
// 报警配置
// ==========================================

// ============================================================================
// 获取报警类别列表
// ============================================================================
// 只有在报警类别列表中的类别才会触发报警
// 例如：配置为 ["person", "helmet"]，则只有检测到人或安全帽时才报警
// ============================================================================
QStringList ConfigManager::alarmClasses() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonArray arr = root_["alarm"].toObject()["classes"].toArray();
    QStringList list;
    for (const auto &v : arr) {
        list.append(v.toString());
    }
    return list;
}

// 设置报警类别列表
void ConfigManager::setAlarmClasses(const QStringList &classes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject alarm = root_["alarm"].toObject();
    QJsonArray arr;
    for (const auto &s : classes) {
        arr.append(s);
    }
    alarm["classes"] = arr;
    root_["alarm"] = alarm;
}

// ============================================================================
// 电子围栏配置
// ============================================================================
// config.json 中的 geofence 段格式：
// {
//   "geofence": {
//     "enabled": true,                    // 是否启用围栏检测
//     "mode": "inside_alarm",             // 报警模式："inside_alarm"/"outside_alarm"
//     "channels": {                       // 各通道围栏配置
//       "0": {                            // 通道 0（通道 1）
//         "shapes": [                     // 围栏形状列表
//           {
//             "type": "rectangle",        // 形状类型："rectangle"/"polygon"
//             "points": [[100,50],[300,200]]  // 顶点坐标（overlay 像素坐标）
//           }
//         ]
//       }
//     },
//     "alarmClasses": ["person"]          // 触发围栏报警的检测类别
//   }
// }
//
// 坐标系说明：
//   - 围栏坐标是在 overlay widget 像素空间中绘制的
//   - 检测时需要将坐标映射到原始视频空间（通过 overlaySize 缩放比例）
// ============================================================================

/**
 * @brief 检查电子围栏是否启用
 * @return: true=启用，false=禁用（默认 false）
 */
bool ConfigManager::geofenceEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["enabled"].toBool(false);
}

/**
 * @brief 设置电子围栏启用状态
 * @param enabled: true=启用围栏检测，false=禁用
 */
void ConfigManager::setGeofenceEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["enabled"] = enabled;
    root_["geofence"] = gf;
}

/**
 * @brief 获取围栏报警模式
 * @return: "inside_alarm"（围栏内报警，默认）或 "outside_alarm"（围栏外报警）
 */
QString ConfigManager::geofenceMode() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["mode"].toString("inside_alarm");
}

/**
 * @brief 设置围栏报警模式
 * @param mode: "inside_alarm"（围栏内报警）或 "outside_alarm"（围栏外报警）
 */
void ConfigManager::setGeofenceMode(const QString &mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["mode"] = mode;
    root_["geofence"] = gf;
}

/**
 * @brief 获取所有通道的围栏配置
 * @return: QJsonObject，key 为通道号字符串（"0","1","2","3"），value 为该通道的围栏形状列表
 */
QJsonObject ConfigManager::geofenceChannels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["channels"].toObject();
}

/**
 * @brief 设置所有通道的围栏配置
 * @param channels: QJsonObject，key 为通道号字符串，value 为围栏形状列表
 */
void ConfigManager::setGeofenceChannels(const QJsonObject &channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["channels"] = channels;
    root_["geofence"] = gf;
}

/**
 * @brief 获取触发围栏报警的检测类别列表
 * @return: 类别名称列表（如 ["person", "helmet"]），默认 ["person"]
 */
QStringList ConfigManager::geofenceAlarmClasses() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonArray arr = root_["geofence"].toObject()["alarmClasses"].toArray();
    QStringList result;
    for (const auto &v : arr) {
        QString s = v.toString().trimmed();
        if (!s.isEmpty()) result.append(s);
    }
    if (result.isEmpty()) {
        result.append("person");  // 默认触发类别
    }
    return result;
}

/**
 * @brief 设置触发围栏报警的检测类别列表
 * @param classes: 类别名称列表（如 ["person", "helmet"]），空列表时使用默认 ["person"]
 */
void ConfigManager::setGeofenceAlarmClasses(const QStringList &classes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    QJsonArray arr;
    for (const auto &s : classes) {
        if (!s.trimmed().isEmpty()) arr.append(s.trimmed());
    }
    gf["alarmClasses"] = arr;
    root_["geofence"] = gf;
}

// ============================================================================
// 数据库配置
// ============================================================================
// config.json 中的 database 段格式：
// {
//   "database": {
//     "storeDetections": true,        // 是否将报警相关的检测数据存入 detections 表
//     "detectionRetentionDays": 30    // 检测数据保留天数（超过自动清理）
//   }
// }
// ============================================================================

/**
 * @brief 是否启用检测数据存储
 * @return: true=将报警相关的检测数据存入 detections 表，false=只存 alarms 表
 */
bool ConfigManager::storeDetections() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return storeDetections_;  // 返回缓存值
}

/**
 * @brief 设置是否启用检测数据存储
 * @param enabled: true=启用，false=禁用
 */
void ConfigManager::setStoreDetections(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject db = root_["database"].toObject();
    db["storeDetections"] = enabled;
    root_["database"] = db;
    storeDetections_ = enabled;  // 更新缓存
}

/**
 * @brief 获取检测数据保留天数
 * @return: 保留天数（默认 30 天）
 */
int ConfigManager::detectionRetentionDays() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return detectionRetentionDays_;  // 返回缓存值
}

/**
 * @brief 设置检测数据保留天数
 * @param days: 保留天数
 */
void ConfigManager::setDetectionRetentionDays(int days)
{
    std::lock_guard<std::mutex> lock(mutex_);
    days = qBound(1, days, 365);  // 限制范围 1 ~ 365 天
    QJsonObject db = root_["database"].toObject();
    db["detectionRetentionDays"] = days;
    root_["database"] = db;
    detectionRetentionDays_ = days;  // 更新缓存
}
