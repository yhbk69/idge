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
#include <QList>
#include "runtime_paths.h"
#include <QJsonObject>
#include <QJsonArray>
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
    // 该模型的置信度覆盖；**负数 = 未配置**（跟随全局 detect.conf_threshold）。
    // 不能用 0.0 当"未配置"：0.0 在语义上是一个合法（虽然危险）的阈值，
    // 而 QJsonValue::toDouble() 对缺键也返回 0.0，所以解析时必须先 contains() 判键。
    float confThreshold = -1.f;
};

/**
 * @brief 一路视频流的完整定义（config.json 的 video.sources[]，通道按 id 引用它）
 *
 * 为什么单独一张表：同一只摄像头/同一路 rtsp 会被多路通道复用，地址与采集参数写在
 * 各通道里就会"改一处漏三处"；而分辨率/帧率/输入格式本来就是**流的属性**，
 * 不是通道的属性（通道决定的是挂哪几个模型）。口径照 caichao 版 idge.json 的
 * `video.sources[]`，字段则是本仓真能接进 FFmpeg 打开参数的那些（见 ffmpeg_video_decoder）。
 *
 * 三个"不指定"的约定：width/height<=0、fps<=0、inputFormat 空——都回落旧口径
 * （摄像头尺寸走全局 smartRecognition.camera_size、帧率 30、输入格式三档协商），
 * 这样老配置一条都不用改也能继续跑。
 */
struct VideoSourceCfg
{
    QString id;              ///< 注册表主键，通道靠它引用；空 = 非注册表条目
    QString type;            ///< camera / file / rtsp / rtmp；空 = 按 url 前缀嗅探（旧口径）
    QString url;             ///< 设备节点、本地文件或流地址
    QString note;            ///< 备注（界面下拉显示用）
    int width = 0;           ///< <=0 = 不指定；只对 camera 生效（其它源以流自身分辨率为准）
    int height = 0;          ///< 同上
    int fps = 0;             ///< <=0 = 不指定；只对 camera 生效（其它源的帧率从流里探）
    QString inputFormat;     ///< 仅 camera：mjpeg / nv12 / yuyv；空 = 三档协商
    bool loop = false;       ///< 仅 file：读到结尾后回到开头继续放
    bool hardwareDecode = true; ///< false = 走软解（不建 rkmpp 设备，帧是 CPU 内存的）
    int timeoutMs = 0;       ///< <=0 = 打开/探测不设墙上时钟超时（旧行为：只受 stop 打断）
    int reconnectMs = 0;     ///< <=0 = 断流/打开失败就不再接（旧行为）
};

/**
 * @brief 一个视频通道的配置（config.json 的 video.channels[下标=通道号]）
 */
struct VideoChannelCfg
{
    QString source;      ///< 内联视频源（旧口径）：本地文件 / 设备节点 / RTSP
    QString sourceId;    ///< 引用 video.sources[] 的 id；非空且命中时**优先于 source**
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

    /**
     * @brief 通道实际要打开的地址：**先按 source_id 查注册表，查不到再用内联 source**
     * @return 两者都没有返回空串（界面据此显示"视频待接入"）
     * @note 悬空的 source_id（非空但注册表里没有）**不回退**到内联 source：
     *       "以为选的是流 A，其实悄悄用了旧地址"比"这一路黑屏 + 一条告警"更难查。
     */
    QString channelSource(int ch) const;
    void setChannelSource(int ch, const QString &source);

    /** @brief 通道引用的流 id（未用注册表时为空串） */
    QString channelSourceId(int ch) const;
    void setChannelSourceId(int ch, const QString &id);

    /**
     * @brief 通道里内联写的 source 原值（旧口径那个键），**不做解析**
     *
     * 只给界面用：设置页要靠它决定下拉里要不要列"内联地址"那一项。
     * "这一路究竟开什么"仍然只有 resolveChannelSource() 一处口径，别拿这个值去开流。
     */
    QString channelInlineSource(int ch) const;

    /**
     * @brief 解析通道最终要用的那条流（地址 + 全部打开参数），一条解析口径
     *
     * 三层兜底：① source_id 命中注册表 → 那条目；② 否则内联 source 非空 → 合成一条
     * （type 按 url 前缀嗅探，其余字段取默认，等价于改动前的行为）；③ 都空 → 返回空 cfg。
     * 所有"这路要开什么"的消费者（解码、界面、将来的 /api/streams）都走这里，
     * 不要在别处再拼一遍 if-else。
     */
    VideoSourceCfg resolveChannelSource(int ch) const;

    // ========== 视频流注册表（video.sources[]，通道按 id 引用）==========

    /** @brief 注册表全部条目（数组顺序即界面下拉顺序） */
    QList<VideoSourceCfg> videoSources() const;

    /** @brief 注册表里出现过的 id（可能含重复 id，见 setVideoSource 的收口说明） */
    QStringList videoSourceIds() const;

    /** @brief 按 id 取一条流；不存在时返回空 cfg（id 与 url 均为空） */
    VideoSourceCfg videoSource(const QString &id) const;

    /**
     * @brief 单条 `video.sources[]` 元素的解析口径（键名与归一规则的唯一事实源）
     *
     * 公开成静态是为了让"手里只有另一份 JSON 文档"的消费者也能复用同一判据，
     * 不必先 `load()` 单例：GB28181 的 `Gb28181Config::load()` 在 `main()` 里跑在
     * `frmMain` 之前（那时单例还是空的），它自己读的那份文档里有 `video.sources[]`。
     */
    static VideoSourceCfg videoSourceFromJson(const QJsonObject &o);

    /** @brief 在**给定的**根对象里按 id 查一条流；不命中返回空 cfg（判据与 videoSource() 同一条） */
    static VideoSourceCfg videoSourceInRoot(const QJsonObject &root, const QString &id);

    /**
     * @brief 新增或按 id 覆盖一条流（id 已存在则原地替换，不存在则追加）
     * @return false = id 或 url 为空，调用方没给够主键/地址，配置不改动
     * @note 只按 id 找**第一条**匹配项替换，并把重复 id 的后续条目一并删掉：
     *       手改 JSON 很容易留下同 id 的两条，若两处都能命中，行为就取决于数组顺序。
     */
    bool setVideoSource(const VideoSourceCfg &source);

    /**
     * @brief 删除一条流，并把引用它的通道的 source_id 一起清空
     * @note 与 removeModelParam() 同一政策：删条目和删引用一次做干净，
     *       分开改就会造出一堆"悬空 id + 黑屏"的通道。
     */
    void removeVideoSource(const QString &id);

    /**
     * @brief 有多少处正在引用这条流（删之前给界面确认用）
     *
     * 计数包含两类消费者：`video.channels[].source_id` 与
     * `gb28181.streams[].source_id`（国标侧一条流被引用时删掉它，平台点流就会拿不到地址）。
     */
    int videoSourceReferrerCount(const QString &id) const;

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
     * @brief 该模型的置信度覆盖（video.models[<id>].conf_threshold）
     * @return 负数 = 未配置，表示该模型跟随全局 detect.conf_threshold
     */
    float modelConfThreshold(const QString &libId) const;

    /** @brief 写该模型的置信度覆盖；控件档位之外仍在此夹到 [0.05, 1.0] */
    void setModelConfThreshold(const QString &libId, qreal val);

    /**
     * @brief 删掉该模型的置信度覆盖键（回到"跟随全局"）
     * 说明：不能拿 setModelConfThreshold(-1) 表达"未配置"——那会把 -1 写进 JSON 存盘，
     *       配置文件里出现负阈值比缺键更难解释；删除原语也只有这一个语义。
     *       removeModelParam() 不能用：它连 alarm_class 和整条通道引用一起删。
     */
    void clearModelConfThreshold(const QString &libId);

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
     * @brief 全局置信度阈值（0.05 ~ 1.0），默认 0.25
     * 说明：它是回落链的**底座**——`video.models[<id>].conf_threshold` 缺键的模型都用它。
     *       2026-10-31 之前这两个键是假开关（写盘、UI 可改、推理不读），现已接通，
     *       生效路径见 `ModelRegistry::resolveChannelSlots()`（唯一解析点）。
     */
    double confThreshold() const;

    /**
     * @brief 设置全局置信度阈值（在此夹到 [0.05, 1.0]）
     */
    void setConfThreshold(double val);

    /**
     * @brief 全局 NMS 阈值（0.0 ~ 1.0），默认 0.45；**只有全局档**，无每模型覆盖
     * 说明：NMS 不设下限——阈值调低是"更激进地合并重叠框"，方向上是变少而不是变多，
     *       不存在置信度那种"预筛失效→候选爆炸"的失败模式，所以不给它编一个假护栏。
     */
    double nmsThreshold() const;

    /**
     * @brief 设置全局 NMS 阈值（夹到 [0.0, 1.0]）
     */
    void setNmsThreshold(double val);

    // 注：detect.class_num 与 detect.threads 两个键没有任何消费者，
    // 对应的 getter 已删除——类别数由每个模型的标签决定，
    // 推理线程数 = 各通道模型数之和（一个模型一个 PpeTask 线程）。

    // ========== 电子围栏配置 ==========

    bool geofenceEnabled() const;
    void setGeofenceEnabled(bool enabled);

    // 退役键：报警规则改由每个围栏形状自带的 role 决定（见 geofence/FenceRole）。
    // 只有 FenceManager::loadFromConfig 读它一次，用于给老配置里缺 role 的形状补角色。
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

    /** @brief 围栏报警节流下限（秒，默认 10）——每通道每角色每类别窗口内只出一条；
     *         "站着不动只报一次"由解码器的进入沿事件表保证，本窗口只兜沿抖动 */
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
    // enabled 默认 true —— 报警关联片段是需求验收项（requirements.md §三①）；
    // 代价是 4 路常驻硬编占用 VPU，不想要就在 JSON 里关掉。

    /** @brief 是否启用报警片段录制（默认 true） */
    bool recordingEnabled() const;

    /** @brief 编码格式："hevc"（默认，hevc_rkmpp）或 "h264"（h264_rkmpp） */
    QString recordingCodec() const;

    /** @brief 环形缓冲区时长（秒，默认 8）——决定能回溯多久，须 ≥ 前3+后2 窗口再加关键帧对齐余量 */
    int recordingBufferSeconds() const;

    /** @brief 编码码率（bps，默认 2000000，CBR） */
    int recordingBitRate() const;

    /** @brief 关键帧间隔（秒，默认 1）——越小报警片段起点对齐误差越小 */
    int recordingGopSeconds() const;

    // ========== 报警上行配置（P0-1）==========
    // 同样全是只读 getter：值在 AlarmUplink::attach() 时一次性读取，运行期改动不重建状态机。
    // enabled 默认 false —— 这是往外发数据，且当前链路明文 ws + 零鉴权，必须人来打开
    //（理由与 recording 相反，见 ConfigManager.cpp 的 makeAlarmUplinkDefaults）。

    /** @brief 是否启用报警上行（默认 false） */
    bool alarmUplinkEnabled() const;

    /** @brief 第一段（报警信息）是否带截图传输计划（默认 true） */
    bool alarmUplinkSendSnapshot() const;

    /** @brief 第二段是否发视频片段（默认 true；recording.enabled=false 时降级为不发） */
    bool alarmUplinkSendClip() const;

    /** @brief 二进制帧分块大小（字节，默认 262144，范围 64KB~1MB） */
    int alarmUplinkChunkSize() const;

    /** @brief 等 uplink_ack 的超时（毫秒，默认 15000；仅 expect_ack=true 时有意义） */
    int alarmUplinkAckTimeoutMs() const;

    /** @brief 同一项最多重投几次（默认 3），超过判 failed 并打 warning */
    int alarmUplinkMaxAttempts() const;

    /** @brief 待确认队列上限（默认 16），满了丢最旧 */
    int alarmUplinkMaxPending() const;

    /** @brief 是否要求对端回 uplink_ack 才算完成（默认 false） */
    bool alarmUplinkExpectAck() const;

    /** @brief 媒体交付方式："url"（默认，本机 http 文件服务出地址）| "ws_binary"（WS 二进制分块帧） */
    QString alarmUplinkMediaTransport() const;

    /** @brief 内嵌 http 文件服务监听端口（默认 8090，范围 1024~65535；板端特权口 bind 不上） */
    int alarmUplinkMediaHttpPort() const;

    /** @brief 媒体地址前缀（默认空 = 自动取本机非回环 IPv4 + 实际绑定端口） */
    QString alarmUplinkMediaBaseUrl() const;

    // ========== pad_server 配置（Pad 接口 v1 的入站服务端，本机是服务端）==========

    /** @brief 是否启用 Pad 服务端（默认 true，2026-11-04 起；显式写 false 才关） */
    bool padServerEnabled() const;

    /** @brief Pad WS 监听端口（默认 8900，范围 1024~65535；8899 按 A4 留给 `/api/` HTTP） */
    int padServerPort() const;

    /** @brief Pad WS 路径（默认 "/"，只进日志——QWebSocketServer 不按路径筛连接） */
    QString padServerPath() const;

    /** @brief hello_ack.deviceId（默认空 = 由 pad::defaultDeviceId() 按 /etc/machine-id 派生，
     *         该键**一定会出现在 hello_ack**，因为 §13.1.3 规定它必填；口径见确认单 A17） */
    QString padServerDeviceId() const;

    /** @brief 证据 URL 前缀覆盖（默认空 = 用内嵌 http 服务的自动地址） */
    QString padServerMediaBaseUrl() const;

    /** @brief 一次 sync_result 的条数上限（默认 500，范围 1~5000） */
    int padServerSyncLimit() const;

    // ========== qr_login 配置（扫码登录的校验档，见 plan/qr-login-edge-v1.md §6）==========

    // 上面六个键的 setter（2026-11-05 起有，给调试设置页用）。写内存后由调用方 save()；
    // 端口/sync_limit 在这里就 qBound，避免界面层各自判一次范围。
    void setPadServerEnabled(bool enabled);
    void setPadServerPort(int port);
    void setPadServerPath(const QString &path);
    void setPadServerDeviceId(const QString &deviceId);
    void setPadServerMediaBaseUrl(const QString &baseUrl);
    void setPadServerSyncLimit(int limit);

    /** @brief 本机当前施工计划 ID（默认空 = 不比对）。非空时 plan.taskId 必须与它相等，
     *         否则按 §6.6 拒绝登录——这是"设备绑定到某个计划"的唯一入口。
     * @note §6.4 有效期与 §6.5 验签本轮**不做**（2026-11-04 裁定），所以没有对应开关。 */
    QString qrLoginExpectedTaskId() const;

    // ========== pad_http 配置（Pad 接口 v1 的 HTTP `/api/` 面，本机是服务端）==========
    // 与 pad_server 是**两台独立服务端、两个独立的口**：那台是 WS 报警面（8900），
    // 这台是 HTTP `/api/`（8899，§4 处置 / §5 围栏 / §6 视频 / §7 设备）。
    // 两段都有 enabled/port，但互不影响：调试页上各有一个「保存并应用」，各管各的重启。

    /** @brief 是否启用 HTTP `/api/` 面（默认 **true**，与 pad_server 同一判断：
     *         这台设备的用途就是被 Pad 连。⚠ 这是明文零鉴权的**写**面，
     *         局域网内任何设备都能改报警状态与围栏；不想要就显式写 false，
     *         关掉是彻底不 bind，不是"bind 了但拒绝"。 */
    bool padHttpEnabled() const;

    /** @brief HTTP 监听端口（默认 8899，范围 1024~65535；A4 分配：8899=`/api/`、8900=WS） */
    int padHttpPort() const;

    /** @brief 单个请求正文的字节上限，单位 MB（默认 26 = §4.2 五张 5MB + JSON 余量；1~100） */
    int padHttpMaxBodyMb() const;

    /** @brief §6.1 的 liveUrl 是否保留视频源里的 `user:pass@`（默认 **false** = 剥掉）。
     *         打开它等于把每台摄像头的口令发给局域网里任何一台设备；
     *         只有确认 Pad 那边必须带凭据才播得起来时才开。 */
    bool padHttpExposeStreamCredentials() const;

    /** @brief 围栏下发时是否渲染合成画面 PNG（默认 true）。
     *         §5.3 要求 apply 必返 frameUrl；关掉它 apply 会回 503/3003，而不是给一个假地址。 */
    bool padHttpFenceFrames() const;

    /** @brief §4.1 的处置选项数组（元素 {id,label,enabled,sortOrder}）。
     *         **空数组 = 用代码内置那两条**（规约 §4.1 示例的原文案）；写出来就整组替换，
     *         现场改措辞不用重新编译。 */
    QJsonArray padHttpDisposeOptions() const;

    void setPadHttpEnabled(bool enabled);
    void setPadHttpPort(int port);
    void setPadHttpMaxBodyMb(int megabytes);
    void setPadHttpExposeStreamCredentials(bool expose);
    void setPadHttpFenceFrames(bool render);
    void setPadHttpDisposeOptions(const QJsonArray &options);

    // ========== Pad RTSP 直播（带检测框视频） ==========
    // 与 pad_http/pad_server 是第三台独立服务：本机把画好检测框的画面硬编码后，
    // 由 ZLM 分发成 RTSP，§6.1 的 liveUrl 优先指向它（详见 src/media/server/rtsp_live_service.h）。

    /** @brief 是否启用 RTSP 直播分发（默认 **true**；关掉则 liveUrl 回落到原始视频源地址） */
    bool padRtspEnabled() const;

    /** @brief RTSP 监听端口（默认 8554，范围 1024~65535；554 需 root，默认避开） */
    int padRtspPort() const;

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

    /**
     * @brief 智能识别/扫码摄像头的采集尺寸，形如 "1280x720"
     *
     * 这是"帧率 vs 看得清"的唯一旋钮，三处一起受影响：摄像头每帧的 CPU 颜色转换量、
     * 喂给 ZBar 的像素量、OCR 抓下来的那张静图分辨率。默认 1280x720 —— 现场码贴在
     * 一臂距离时中心区域仍有几百像素，解得出；铭牌 OCR 也还够用。
     * 帧率还是低就往下降（如 "960x540"），OCR 认不清字就往回升（"1920x1080"）。
     *
     * 只有 v4l2 设备节点用得到（文件/RTSP 源的分辨率由源自己决定）。填得不合法
     * （格式错、超范围）会回退默认值并打一条 qWarning，不静默。
     * 键缺省时同样回默认，与本模块其他键一致：有 getter、出厂默认会写入，但设置页暂无控件。
     */
    QString smartRecognitionCameraSize() const;

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

    /**
     * @brief 老配置缺少 alarm_uplink 段时补齐默认值（load 内持锁调用，勿单独使用）
     *
     * 与 migrateRecordingDefaults 同口径：只为让开关落进文件可见可改，
     * 已存在 alarm_uplink 段时不做任何修改（现场配置不会被启动流程覆盖）。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migrateAlarmUplinkDefaults();

    /**
     * @brief 老配置缺少 pad_server 段时补齐默认值（load 内持锁调用，勿单独使用）
     *
     * 与 migrateAlarmUplinkDefaults 同口径；默认 enabled=false，补齐不会带来任何行为变化，
     * 只是让这个入站开关在现场 config.json 里可见可改。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migratePadServerDefaults();

    /**
     * @brief 老配置缺少 pad_http 段时补齐默认值（load 内持锁调用，勿单独使用）
     *
     * 与 migratePadServerDefaults 同口径，只为让"这台有没有开 HTTP 写面"在现场文件里
     * 可见可改。默认 enabled=true，所以**补齐本身就会开始监听 8899** ——
     * 这与 pad_server 那句"出厂默认开"是同一个决定，风险与理由一起写在 README 里。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migratePadHttpDefaults();

    /**
     * @brief 老配置缺少 pad_rtsp 段时补齐默认值（load 内持锁调用，勿单独使用）
     *
     * 默认 enabled=true、port=8554。补齐本身会让 §6.1 的 liveUrl 改指本机 RTSP 直播源
     * （带检测框画面），而不再是原始摄像头地址。
     * @return true=root_ 有改动（调用方负责 saveUnsafe 落盘）
     */
    bool migratePadRtspDefaults();

    /**
     * @brief 老配置没有 video.sources 时补一个空数组（load 内持锁调用）
     *
     * 只为让这一段落进文件可见可改；**不**把各通道已有的内联 source 抽成条目——
     * 那等于替现场决定"这几路算什么流、叫什么名"，而且抽完 id 就成了中文路径的哈希，
     * 反而更难维护。想用注册表的人在设置页的下拉里选一次即可。
     */
    bool migrateVideoSourcesDefaults();

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
