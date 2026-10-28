#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

/**
 * @file ConfigManager.h
 * @brief 统一配置管理器 - 单例模式
 *
 * 所有配置集中在 config.json 中管理，包括：
 *   - 每个视频通道一段（源 + 备注 + 是否启用 + 有序模型列表）
 *   - 模型参数表（按模型库目录名索引：报警类别、标签覆盖、备注）
 *   - 检测参数（置信度阈值、NMS阈值、类别数、线程数）
 *   - 电子围栏整段（geofence：触发类别、节流窗口、各通道形状）
 *   - 报警视频录制、数据库、WebSocket、国标
 *
 * 使用方式：
 *   ConfigManager::instance().load(RuntimePaths::configFile());   // 启动时加载
 *   ConfigManager::instance().setChannelSource(0, path); // 修改配置
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
 *      ⚠ 同理，load() 内部的迁移函数**不得**回调 ModelRegistry——它的
 *      referenceCount()/resolveChannelSlots() 会回头取本类锁，直接死锁。
 *      迁移只做纯字符串换算（见 pathToLibraryId）。
 *   3. 缺省值兜底：getter 多用 toDouble(0.25)/toInt(80)/toString("inside_alarm") 等
 *      带默认值的形式，缺键时返回兜底值而非崩溃；但 save() 打开文件失败会被静默忽略
 *      （仅 load 时 qWarning），落盘结果需自行保证可写路径。
 *   4. 通道号一律 0-based：video.channels 是数组，**下标即通道号**（0~3），
 *      与解码器 channel_、geofence.channels 的键、openVideo(0..3) 同口径。
 *      条目里的 "channel" 字段只是给人看的，程序不读它。
 */

#include <QString>
#include <QStringList>
#include "runtime_paths.h"
#include <QJsonObject>
#include <mutex>

/**
 * @brief 一个模型的配置参数（config.json 的 video.models[<模型库目录名>]）
 *
 * 报警类别属于"模型的参数"而非"槽位的参数"：同一个模型在任何通道的
 * 任何槽位上，触发的都是同一个类别，换槽位不换行为。
 * 路径不在这里配——模型库 model/library/<id>/{model.rknn,labels.txt} 是
 * 唯一事实源，config 里不出现 .rknn/labels.txt 字符串（配置里写死路径
 * 会让仓库搬迁、模型换库都要重配，且与启动迁移互相拉锯）。
 */
struct VideoModelParam
{
    QString alarmClass;  ///< 触发报警的类别名；空 = 只画框不报警
    QString label;       ///< 标签文件覆盖（一般留空，用库内 labels.txt）
    QString note;        ///< 备注
};

/**
 * @brief 一个视频通道的配置（config.json 的 video.channels[下标=通道号]）
 */
struct VideoChannelCfg
{
    QString source;      ///< 视频源：本地文件 / 设备节点 / RTSP
    QString note;        ///< 通道备注（四宫格 OSD 用）
    bool enabled = true; ///< false = 启动时不打开这一路
    QStringList modelIds;///< 有序模型库 id 列表，顺序即级联槽位顺序（= result_id）
};

class ConfigManager
{
public:
    /**
     * @brief 获取单例实例
     */
    static ConfigManager &instance();

    /**
     * @brief 从 JSON 文件加载配置
     * @param path 配置文件路径，默认 data/config.json
     */
    void load(const QString &path = RuntimePaths::configFile());

    /**
     * @brief 将当前配置保存到 JSON 文件
     */
    void save();

    // ========== 通道配置（video.channels，下标即通道号 0..3）==========

    /** @brief 通道数（固定 4 路，与 frmVideoWindow 的四宫格一致） */
    int channelCount() const { return 4; }

    /** @brief 通道完整配置（缺键字段取 VideoChannelCfg 的默认值） */
    VideoChannelCfg channelCfg(int ch) const;

    /** @brief 通道视频源（本地文件/设备节点/RTSP），通道号非法返回空串 */
    QString channelSource(int ch) const;
    void setChannelSource(int ch, const QString &source);

    /** @brief 通道备注（四宫格 OSD），通道号非法返回空串 */
    QString channelNote(int ch) const;
    void setChannelNote(int ch, const QString &note);

    /** @brief 该通道是否打开（缺省 true；false 只影响启动时是否 openVideo） */
    bool channelEnabled(int ch) const;
    void setChannelEnabled(int ch, bool enabled);

    /** @brief 该通道的有序模型库 id 列表（列表顺序 = 级联槽位顺序 = result_id） */
    QStringList channelModelIds(int ch) const;
    void setChannelModelIds(int ch, const QStringList &ids);

    // ========== 模型参数（video.models，键 = 模型库目录名）==========

    /** @brief 参数表里出现过的库 id（QJsonObject 按键升序） */
    QStringList modelParamKeys() const;

    /** @brief 某库 id 的参数；无条目时 alarmClass/label/note 均为空 */
    VideoModelParam modelParam(const QString &libId) const;

    /** @brief 该模型的报警类别；"" = 只画框不报警（无条目也是 ""） */
    QString modelAlarmClass(const QString &libId) const;
    void setModelAlarmClass(const QString &libId, const QString &className);

    /** @brief 该模型的标签文件覆盖（空 = 用库内 labels.txt） */
    void setModelLabel(const QString &libId, const QString &label);

    /** @brief 该模型的备注 */
    void setModelNote(const QString &libId, const QString &note);

    /**
     * @brief 从参数表删除某模型的条目，并从所有通道的模型列表里一并剔除
     * @note 参数表条目 = 这个模型"要不要出现在配置里"，通道列表 = "哪几路在用"，
     *       两处分开改会让人留下悬空引用，所以这里一次做干净。
     */
    void removeModelParam(const QString &libId);

    /**
     * @brief 哪些通道的模型列表引用了该库 id（供 UI 显示"被引用通道"与删除守卫）
     * @return 通道号列表（0-based），无人引用返回空
     */
    QList<int> modelParamUsers(const QString &libId) const;

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

    // 注：detect.class_num 与 detect.threads 两个键没有任何消费者，
    // 对应的 getter 已删除——类别数由每个模型的标签决定，
    // 推理线程数 = 各通道模型数之和（一个模型一个 PpeTask 线程）。

    // ========== 电子围栏配置 ==========

    bool geofenceEnabled() const;
    void setGeofenceEnabled(bool enabled);

    QString geofenceMode() const;
    void setGeofenceMode(const QString &mode);

    QJsonObject geofenceChannels() const;
    void setGeofenceChannels(const QJsonObject &channels);

    /**
     * @brief 围栏触发类别（单值，默认 "person"）
     *
     * 围栏报警不属于级联模型的任何槽位，判定只看"这一路里是不是人"，
     * 所以这里是一个类别名而不是列表，也不参与 video.models[].alarm_class
     * 那套报警门限（详见 plan/roadmap.md 的围栏条目）。
     * 老配置里的 geofence.alarmClasses 列表由 migrateSchemaV2() 折叠成单值。
     */
    QString geofenceAlarmClass() const;
    void setGeofenceAlarmClass(const QString &cls);

    /** @brief 围栏报警节流窗口（秒，默认 10）——每通道每类别窗口内只出一条 */
    int geofenceThrottleSeconds() const;
    void setGeofenceThrottleSeconds(int seconds);

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

    // ========== 报警视频录制配置 ==========
    // 全部键可选：缺失即返回下面的默认值，现场老配置不改也能启动。
    // enabled 默认 false —— 硬件编码器会常驻占用 VPU，必须由人显式打开。

    /** @brief 是否启用报警片段录制（默认 false） */
    bool recordingEnabled() const;

    /** @brief 编码格式："hevc"（默认，hevc_rkmpp）或 "h264"（h264_rkmpp） */
    QString recordingCodec() const;

    /** @brief 环形缓冲区时长（秒，默认 5）——决定能回溯多久 */
    int recordingBufferSeconds() const;

    /** @brief 编码码率（bps，默认 2000000，CBR） */
    int recordingBitRate() const;

    /** @brief 关键帧间隔（秒，默认 1）——越小报警片段起点对齐误差越小 */
    int recordingGopSeconds() const;

    // ========== WebSocket 配置 ==========

    /** @brief WebSocket 服务器地址（如 "ws://192.168.1.100:8765"） */
    QString wsServerUrl() const;
    void setWsServerUrl(const QString &url);

    /** @brief 心跳间隔（秒，默认 30） */
    int wsHeartbeatInterval() const;

    /** @brief 断线重连间隔（秒，默认 5） */
    int wsReconnectInterval() const;

    // ========== 智能识别配置 ==========

    /**
     * @brief 智能识别页的取流地址（QR / OCR / 点名 / 盘点 共用同一键）
     *
     * 值直接交给 FFmpegVideoDecoder::start() / CameraPreviewDecoder::start()，
     * 经 avformat_open_input 自动探测，因此本机设备节点、本地视频文件、RTSP
     * 地址都可以用（后两者仅便于无摄像头环境下自测；点名/盘点的预览解码器只认
     * v4l2 设备节点，填文件或 RTSP 打不开）。设备节点依赖 main() 里
     * initializeMediaRuntime() 注册的 libavdevice v4l2 输入源，缺那次初始化
     * 只能开文件和 RTSP。
     * 键缺省或为空时实探测摄像头节点：优先支持 MJPEG 的（USB UVC 摄像头），
     * 一个都没有再退取支持 NV12/YUYV 的（RK3588 的 CSI 摄像头经 rkisp_mainpath
     * 出流、不提供 MJPEG，本板落在 /dev/video22）；探测不到才退回 /dev/video0——
     * 节点号会随插拔/接线/内核变化漂移，所以不再写死曾经板端使用的 /dev/video41。
     * 注意本键与国标推流各自的取流地址互不协调，同一设备被两处同时打开会失败。
     */
    QString smartRecognitionCamera() const;

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

    /**
     * @brief schema V1（全局 5 槽 cascade）→ V2（按通道 + 模型参数表）一次性迁移
     *
     * 照搬 V1 的**有效语义**而不是字段名：槽 i 的路径来自
     * i==0 ? model.path : cascade.models[i].path，而报警类/标签/备注一律取
     * cascade.models[i]（当年 buildCascadeTasks 用 cfgIdx+1 反查报警类就是这个配对，
     * 迁移时顺手"改正"它会让现场报警行为无声变化）。
     * 迁移结果：非空槽归并成 video.models 参数表（同库多槽报警类不同则取非空者并打日志），
     * video.channel1..4 + video.notes 变成 4 个 channels 条目，模型列表**广播到每个通道**
     * （V1 本就是四路共用一套，等价），geofence.alarmClasses 折叠成单值 alarmClass，
     * 旧键（model/cascade/alarm/video.channelN/video.notes）整段删除。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     * @note 内部只做纯字符串换算，**不得**回调 ModelRegistry（它会回头取本类锁 → 死锁）。
     */
    bool migrateSchemaV2();

    /**
     * @brief root_ 是否已是 V2 schema（video.channels 存在）
     * @note 迁移完会删掉全部 V1 键，所以这个判断一旦为真就永久为真（幂等）。
     */
    static bool hasSchemaV2(const QJsonObject &root);

    /**
     * @brief 老配置缺少 recording 段时补齐默认值（load 内持锁调用，勿单独使用）
     *
     * 只为让开关在 config.json 里可见可改，程序本身靠 getter 的默认值兜底；
     * 已存在 recording 段时不做任何修改（现场配置不会被启动流程覆盖）。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migrateRecordingDefaults();

    QString configPath_;   ///< 配置文件路径
    QJsonObject root_;     ///< JSON 根对象
    mutable std::mutex mutex_;  ///< 保护 root_ 的互斥锁

    // 缓存的高频访问值（避免每次 getter 都反序列化 JSON）
    double confThreshold_ = 0.25;      ///< 置信度阈值缓存
    double nmsThreshold_ = 0.45;       ///< NMS 阈值缓存
    bool storeDetections_ = true;      ///< 检测数据存储开关缓存
    int detectionRetentionDays_ = 30;  ///< 检测数据保留天数缓存
};

#endif // CONFIGMANAGER_H
