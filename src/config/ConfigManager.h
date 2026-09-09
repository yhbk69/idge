#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

/**
 * @file ConfigManager.h
 * @brief 统一配置管理器 - 单例模式
 *
 * 所有配置集中在 config.json 中管理，包括：
 *   - 视频通道路径（4路）
 *   - 模型路径、标签路径、输入尺寸、模型类型
 *   - 检测参数（置信度阈值、NMS阈值、类别数、线程数）
 *
 * 使用方式：
 *   ConfigManager::instance().load("config.json");   // 启动时加载
 *   ConfigManager::instance().setVideoChannel(1, path); // 修改配置
 *   ConfigManager::instance().save();                 // 保存到文件
 */

#include <QString>
#include <QStringList>
#include <QJsonObject>

class ConfigManager
{
public:
    /**
     * @brief 获取单例实例
     */
    static ConfigManager &instance();

    /**
     * @brief 从 JSON 文件加载配置
     * @param path 配置文件路径，默认 "config.json"
     */
    void load(const QString &path = "config.json");

    /**
     * @brief 将当前配置保存到 JSON 文件
     */
    void save();

    // ========== 视频通道配置 ==========

    /**
     * @brief 获取指定通道的视频路径
     * @param ch 通道号（1-4）
     */
    QString videoChannel(int ch) const;

    /**
     * @brief 设置指定通道的视频路径
     * @param ch   通道号（1-4）
     * @param path 视频文件路径或RTSP地址
     */
    void setVideoChannel(int ch, const QString &path);

    // ========== 视频通道备注 ==========

    /**
     * @brief 获取指定通道的备注文字
     * @param ch 通道号（1-4），无备注返回空
     */
    QString channelNote(int ch) const;

    /**
     * @brief 设置指定通道的备注文字
     */
    void setChannelNote(int ch, const QString &note);

    // ========== 级联模型配置 ==========

    /**
     * @brief 获取第 idx 个级联模型的路径（1~5），无则返回空串（表示不用）
     */
    QString cascadeModelPath(int idx) const;

    /**
     * @brief 设置第 idx 个级联模型的路径
     */
    void setCascadeModelPath(int idx, const QString &path);

    /**
     * @brief 获取第 idx 个级联模型的备注
     */
    QString cascadeModelNote(int idx) const;

    /**
     * @brief 设置第 idx 个级联模型的备注
     */
    void setCascadeModelNote(int idx, const QString &note);

    /**
     * @brief 获取第 idx 个级联模型的标签文件路径（1~5）
     */
    QString cascadeModelLabel(int idx) const;

    /**
     * @brief 设置第 idx 个级联模型的标签文件路径
     */
    void setCascadeModelLabel(int idx, const QString &label);

    // ========== 模型配置 ==========

    /**
     * @brief 获取 RKNN 模型文件路径
     */
    QString modelPath() const;

    /**
     * @brief 设置 RKNN 模型文件路径
     */
    void setModelPath(const QString &path);

    /**
     * @brief 获取标签文件路径
     */
    QString labelPath() const;

    /**
     * @brief 设置标签文件路径
     */
    void setLabelPath(const QString &path);

    /**
     * @brief 获取模型输入尺寸（如 "640x640"）
     */
    QString inputSize() const;

    /**
     * @brief 获取模型类型（如 "YOLO11"）
     */
    QString modelType() const;

    // ========== 检测参数配置 ==========

    /**
     * @brief 获取置信度阈值（0.0 ~ 1.0）
     */
    double confThreshold() const;

    /**
     * @brief 设置置信度阈值
     */
    void setConfThreshold(double val);

    /**
     * @brief 获取 NMS 阈值（0.0 ~ 1.0）
     */
    double nmsThreshold() const;

    /**
     * @brief 设置 NMS 阈值
     */
    void setNmsThreshold(double val);

    /**
     * @brief 获取检测类别数量
     */
    int classNum() const;

    /**
     * @brief 获取推理线程数
     */
    int threads() const;

    // ========== 报警配置 ==========

    /**
     * @brief 获取触发报警的类别列表
     */
    QStringList alarmClasses() const;

    /**
     * @brief 设置触发报警的类别列表
     */
    void setAlarmClasses(const QStringList &classes);

private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    ConfigManager() {}

    QString configPath_;   ///< 配置文件路径
    QJsonObject root_;     ///< JSON 根对象
};

#endif // CONFIGMANAGER_H
