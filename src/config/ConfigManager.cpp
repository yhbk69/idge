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

        save();  // 保存默认配置
        return;
    }

    // 读取并解析 JSON 文件
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        root_ = doc.object();
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
    QFile file(configPath_);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(root_);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
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
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["path"].toString();
}

// 设置级联模型路径
void ConfigManager::setCascadeModelPath(int idx, const QString &path)
{
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
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["note"].toString();
}

// 设置级联模型备注
void ConfigManager::setCascadeModelNote(int idx, const QString &note)
{
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
    QJsonObject cascade = root_["cascade"].toObject();
    return cascadeModelObject(cascade, idx)["label"].toString();
}

// 设置级联模型标签文件路径
void ConfigManager::setCascadeModelLabel(int idx, const QString &label)
{
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
    return root_["model"].toObject()["path"].toString();
}

// 设置 RKNN 模型文件路径
void ConfigManager::setModelPath(const QString &path)
{
    QJsonObject model = root_["model"].toObject();
    model["path"] = path;
    root_["model"] = model;
}

// 获取标签文件路径
QString ConfigManager::labelPath() const
{
    return root_["model"].toObject()["label"].toString();
}

// 设置标签文件路径
void ConfigManager::setLabelPath(const QString &path)
{
    QJsonObject model = root_["model"].toObject();
    model["label"] = path;
    root_["model"] = model;
}

// 获取模型输入尺寸（格式如 "640x640"）
QString ConfigManager::inputSize() const
{
    return root_["model"].toObject()["input_size"].toString();
}

// 获取模型类型名称（如 "YOLO11"）
QString ConfigManager::modelType() const
{
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
    return root_["detect"].toObject()["conf_threshold"].toDouble(0.25);
}

// 设置置信度阈值
void ConfigManager::setConfThreshold(double val)
{
    QJsonObject detect = root_["detect"].toObject();
    detect["conf_threshold"] = val;
    root_["detect"] = detect;
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
    return root_["detect"].toObject()["nms_threshold"].toDouble(0.45);
}

// 设置 NMS 阈值
void ConfigManager::setNmsThreshold(double val)
{
    QJsonObject detect = root_["detect"].toObject();
    detect["nms_threshold"] = val;
    root_["detect"] = detect;
}

// 获取检测类别数量（默认 80，COCO 数据集）
int ConfigManager::classNum() const
{
    return root_["detect"].toObject()["class_num"].toInt(80);
}

// ============================================================================
// 获取推理线程数
// ============================================================================
// 默认 3，对应 RK3588 的 3 个 NPU 核心
// 每个线程绑定一个 NPU 核心，可以并行推理
// ============================================================================
int ConfigManager::threads() const
{
    return root_["detect"].toObject()["threads"].toInt(3);
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
    QJsonObject alarm = root_["alarm"].toObject();
    QJsonArray arr;
    for (const auto &s : classes) {
        arr.append(s);
    }
    alarm["classes"] = arr;
    root_["alarm"] = alarm;
}
