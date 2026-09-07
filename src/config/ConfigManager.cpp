#include "ConfigManager.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

/**
 * @brief 获取单例实例
 * 使用静态局部变量保证线程安全的懒汉式单例
 */
ConfigManager &ConfigManager::instance()
{
    static ConfigManager mgr;
    return mgr;
}

/**
 * @brief 从 JSON 文件加载配置
 *
 * 如果文件不存在，自动创建默认配置并保存。
 * 默认配置：
 *   - 4路视频通道都指向 "192.mp4"
 *   - 模型路径: model/yolo11n.rknn
 *   - 标签路径: model/coco_80_labels_list.txt
 *   - 置信度阈值: 0.25
 *   - NMS阈值: 0.45
 *
 * @param path 配置文件路径
 */
void ConfigManager::load(const QString &path)
{
    configPath_ = path;
    QFile file(path);

    // 文件不存在时，创建默认配置
    if (!file.exists()) {
        root_["video"] = QJsonObject{
            {"channel1", "192.mp4"},
            {"channel2", "192.mp4"},
            {"channel3", "192.mp4"},
            {"channel4", "192.mp4"}
        };
        root_["model"] = QJsonObject{
            {"path", "model/yolo11n.rknn"},
            {"label", "model/coco_80_labels_list.txt"},
            {"input_size", "640x640"},
            {"type", "YOLO11"}
        };
        root_["detect"] = QJsonObject{
            {"conf_threshold", 0.25},
            {"nms_threshold", 0.45},
            {"class_num", 80},
            {"threads", 3}
        };
        save();
        return;
    }

    // 读取并解析 JSON 文件
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        root_ = doc.object();
        file.close();
    }
}

/**
 * @brief 将当前配置保存到 JSON 文件
 * 使用 Indented 格式写入，便于人工阅读和编辑
 */
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

/**
 * @brief 获取指定通道的视频路径
 * @param ch 通道号，取值 1-4
 * @return 视频文件路径或RTSP地址，通道不存在时返回空字符串
 */
QString ConfigManager::videoChannel(int ch) const
{
    QJsonObject video = root_["video"].toObject();
    return video.value(QString("channel%1").arg(ch)).toString();
}

/**
 * @brief 设置指定通道的视频路径
 * @param ch   通道号，取值 1-4
 * @param path 视频文件路径或RTSP地址
 */
void ConfigManager::setVideoChannel(int ch, const QString &path)
{
    QJsonObject video = root_["video"].toObject();
    video[QString("channel%1").arg(ch)] = path;
    root_["video"] = video;
}

// ==========================================
// 模型配置
// ==========================================

/**
 * @brief 获取 RKNN 模型文件路径
 * @return 模型文件的相对或绝对路径
 */
QString ConfigManager::modelPath() const
{
    return root_["model"].toObject()["path"].toString();
}

/**
 * @brief 设置 RKNN 模型文件路径
 * @param path 模型文件路径
 */
void ConfigManager::setModelPath(const QString &path)
{
    QJsonObject model = root_["model"].toObject();
    model["path"] = path;
    root_["model"] = model;
}

/**
 * @brief 获取标签文件路径
 * @return 标签文件路径（如 coco_80_labels_list.txt）
 */
QString ConfigManager::labelPath() const
{
    return root_["model"].toObject()["label"].toString();
}

/**
 * @brief 设置标签文件路径
 * @param path 标签文件路径
 */
void ConfigManager::setLabelPath(const QString &path)
{
    QJsonObject model = root_["model"].toObject();
    model["label"] = path;
    root_["model"] = model;
}

/**
 * @brief 获取模型输入尺寸
 * @return 格式如 "640x640"
 */
QString ConfigManager::inputSize() const
{
    return root_["model"].toObject()["input_size"].toString();
}

/**
 * @brief 获取模型类型名称
 * @return 如 "YOLO11"
 */
QString ConfigManager::modelType() const
{
    return root_["model"].toObject()["type"].toString();
}

// ==========================================
// 检测参数配置
// ==========================================

/**
 * @brief 获取置信度阈值
 * @return 阈值范围 0.0 ~ 1.0，默认 0.25
 */
double ConfigManager::confThreshold() const
{
    return root_["detect"].toObject()["conf_threshold"].toDouble(0.25);
}

/**
 * @brief 设置置信度阈值
 * @param val 阈值范围 0.0 ~ 1.0
 */
void ConfigManager::setConfThreshold(double val)
{
    QJsonObject detect = root_["detect"].toObject();
    detect["conf_threshold"] = val;
    root_["detect"] = detect;
}

/**
 * @brief 获取 NMS（非极大值抑制）阈值
 * @return 阈值范围 0.0 ~ 1.0，默认 0.45
 */
double ConfigManager::nmsThreshold() const
{
    return root_["detect"].toObject()["nms_threshold"].toDouble(0.45);
}

/**
 * @brief 设置 NMS 阈值
 * @param val 阈值范围 0.0 ~ 1.0
 */
void ConfigManager::setNmsThreshold(double val)
{
    QJsonObject detect = root_["detect"].toObject();
    detect["nms_threshold"] = val;
    root_["detect"] = detect;
}

/**
 * @brief 获取检测类别数量
 * @return 默认 80（COCO数据集）
 */
int ConfigManager::classNum() const
{
    return root_["detect"].toObject()["class_num"].toInt(80);
}

/**
 * @brief 获取推理线程数
 * @return 默认 3（对应 RK3588 的 3 个 NPU 核心）
 */
int ConfigManager::threads() const
{
    return root_["detect"].toObject()["threads"].toInt(3);
}
