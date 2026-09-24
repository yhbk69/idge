#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

/**
 * @file ConfigManager.h
 * @brief 统一配置管理器 - 单例模式
 *
 * 所有配置集中在 config.json 中管理，包括：
 *   - 视频通道路径（4路）+ 通道备注
 *   - 级联模型路径/标签/备注（最多 5 个，主模型占用第 1 槽）
 *   - 模型路径、标签路径、输入尺寸、模型类型
 *   - 检测参数（置信度阈值、NMS阈值、类别数、线程数）
 *   - 报警类别、电子围栏整段（geofence）
 *
 * 使用方式：
 *   ConfigManager::instance().load("config.json");   // 启动时加载
 *   ConfigManager::instance().setVideoChannel(1, path); // 修改配置
 *   ConfigManager::instance().save();                 // 保存到文件
 *
 * 【使用约定·重要】
 *   1. 内存态 vs 落盘：load() 把整个 JSON 读进 root_（内存 QJsonObject）；
 *      所有 setXxx() 只改内存中的 root_，并不会自动写文件——必须显式调用 save()
 *      才落盘。漏调 save() 会导致"改了但重启丢失"。getters 读的是内存 root_，
 *      因此 getter 能立刻看到 setter 的效果，即便尚未 save()。
 *   2. 线程安全：公开接口（load/save/全部 getter/setter）内部由 mutex_ 加锁，
 *      可跨线程调用；但 saveUnsafe() 是"已持有锁时的内部复用版"，外部直接调用
 *      会与 save() 的加锁路径冲突（std::mutex 不可重入，直接调用将死锁），
 *      仅供本类已持锁的成员函数使用。
 *   3. 缺省值兜底：getter 多用 toDouble(0.25)/toInt(80)/toString("inside_alarm") 等
 *      带默认值的形式，缺键时返回兜底值而非崩溃；但 save() 打开文件失败会被静默忽略
 *      （仅 load 时 qWarning），落盘结果需自行保证可写路径。
 */

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <mutex>

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

    // ========== 电子围栏配置 ==========

    bool geofenceEnabled() const;
    void setGeofenceEnabled(bool enabled);

    QString geofenceMode() const;
    void setGeofenceMode(const QString &mode);

    QJsonObject geofenceChannels() const;
    void setGeofenceChannels(const QJsonObject &channels);

    QStringList geofenceAlarmClasses() const;
    void setGeofenceAlarmClasses(const QStringList &classes);

    // ========== 数据库配置 ==========

    /**
     * @brief 是否启用检测数据存储
     * @return: true=将报警相关的检测数据存入 detections 表，false=只存 alarms 表
     * @note 默认 true，仅存报警相关的检测（非所有帧）
     */
    bool storeDetections() const;

    /**
     * @brief 设置是否启用检测数据存储
     * @param enabled: true=启用，false=禁用
     */
    void setStoreDetections(bool enabled);

    /**
     * @brief 获取检测数据保留天数
     * @return: 保留天数（默认 30 天）
     */
    int detectionRetentionDays() const;

    /**
     * @brief 设置检测数据保留天数
     * @param days: 保留天数
     */
    void setDetectionRetentionDays(int days);

private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    ConfigManager() {}

    void saveUnsafe();  ///< 内部不加锁的保存方法
    void updateCache(); ///< 从 root_ 更新缓存值
    /**
     * @brief 旧配置一次性迁移（load 内持锁调用，勿单独使用）
     *
     * 规则：
     *   1) 绝对路径去前缀：/任意前缀/model/xxx → model/xxx（保持仓库可搬迁）
     *   2) 旧平铺模型 model/yolo11{,n,s,m}.rknn → model/library/yolo11X-coco/model.rknn
     *      （仅当库内新文件存在时替换）
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migrateModelPaths();

    QString configPath_;   ///< 配置文件路径
    QJsonObject root_;     ///< JSON 根对象
    mutable std::mutex mutex_;  ///< 保护 root_ 的互斥锁

    // 缓存的高频访问值（避免每次 getter 都反序列化 JSON）
    double confThreshold_ = 0.25;      ///< 置信度阈值缓存
    double nmsThreshold_ = 0.45;       ///< NMS 阈值缓存
    int classNum_ = 80;                ///< 类别数缓存
    int threads_ = 3;                  ///< 推理线程数缓存
    bool storeDetections_ = true;      ///< 检测数据存储开关缓存
    int detectionRetentionDays_ = 30;  ///< 检测数据保留天数缓存
};

#endif // CONFIGMANAGER_H
