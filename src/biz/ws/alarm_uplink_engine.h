#ifndef ALARM_UPLINK_ENGINE_H
#define ALARM_UPLINK_ENGINE_H

/**
 * @file alarm_uplink_engine.h
 * @brief 报警上行的报文组装与待发消息队列（纯逻辑，不依赖 QObject / 不读配置）
 *
 * 两段式上行的第一段在这里成形：报警产生那一刻，抓拍图和视频片段**都还没落盘**
 * （SnapWriter 异步写图、片段要等报警后 2 秒），所以第一段只发 JSON——媒体位置
 * 用 media_id 登记成"预告"（`planned: true`），真实字节数与 sha256 留给后续的
 * media_begin 帧。**媒体字节一律不进 JSON**：url 档（默认）发的是本机 http 文件服务的
 * 地址，ws_binary 档发的是二进制分块帧，两档都只有文本描述走 JSON。
 *
 * 合流：一帧可以产多条报警行，但它们共享同一张抓拍图与同一个片段
 * （ffmpeg_video_decoder.cpp 的 deliverAlarms 按批出一张图一个片段）。引擎按
 * (kind, 批次键) 归并到同一个 media slot：同一份字节只登记一次、只上传一次，
 * alarm_info 里的 `alarm_ids` 告诉对端这些报警共用同一份媒体。
 *
 * 队列：内存队列，上限 maxPending，满了丢最旧并记日志。断线时不丢——pumpOut()
 * 在重连后把没发出去的项原样重发（本轮不做落库 outbox，进程重启即重新开始）。
 *
 * 分层：本类不知道 WebSocket 的存在，只对 UplinkTransport 抽象发文本帧；
 * 也不知道 AlarmManager 的存在，只吃 AlarmInfoInput 这个纯数据子集。
 * 两点合起来让 tests/test_alarm_uplink.cpp 能只链 Qt5::Core 跑起来。
 */

#include <QByteArray>
#include <functional>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <deque>

#include "uplink_transport.h"

/// 协议里的字面量集中在这里：与 Pad 改名只动这一处 + 对应的组装函数
namespace uplink {
inline constexpr const char kTypeAlarmInfo[]     = "alarm_info";      ///< 上行第一段
inline constexpr const char kTypeMediaBegin[]    = "media_begin";      ///< 媒体开始（文本帧）
inline constexpr const char kTypeMediaEnd[]      = "media_end";        ///< 媒体结束（文本帧）
inline constexpr const char kTypeUplinkAck[]     = "uplink_ack";      ///< 下行确认（本轮默认不要求）
inline constexpr const char kTypeMediaFailed[]  = "media_failed";     ///< 媒体承诺落空（两档 transport 都发）
inline constexpr const char kTransferChunks[]    = "binary_chunks";   ///< 媒体传输方式：WS 二进制分块帧
inline constexpr const char kTransferHttp[]      = "http";            ///< 媒体传输方式：本机 http 文件服务出地址
inline constexpr const char kKindSnapshot[]      = "snapshot";        ///< 同时是 alarm_info 子对象键
inline constexpr const char kKindClip[]          = "clip";
inline constexpr const char kMimeJpeg[]          = "image/jpeg";
inline constexpr const char kMimeHevc[]          = "video/H265";
inline constexpr const char kMimeAvc[]           = "video/H264";
inline constexpr const char kMimeMp4[]           = "video/mp4";       ///< url 模式发的是 .mp4 副本
/// media_transport 配置取值。判据只有一处（AlarmUplink::attach 折算成 Settings 里的一个 bool），
/// 引擎内不再分叉，否则两档行为会在两个地方各自漂移
inline constexpr const char kTransportUrl[]      = "url";             ///< 默认
inline constexpr const char kTransportWsBinary[] = "ws_binary";
/// media slot 表上限：键是每个报警批次的文件路径（几乎不重复），超过就淘汰最早登记的
constexpr int kMaxMediaSlots = 64;
/// 片段承诺表上限：一帧可产多条报警行，id 比批次涨得快，单独给一个更大的帽
constexpr int kMaxClipPromises = 256;
/// 抓拍图的预告字节数：报警那一刻文件还没写出来，只能给板端实测均值（~365KB/张）取整
constexpr qint64 kSnapshotExpectedBytes = 368640;
/// 片段的预告就绪时间：post 窗口 2 秒 + dump 与排队余量，只用来让对端有个预期
constexpr int kClipAvailableInMs = 5000;
/// 退避上限：第 n 次重试等 min(2^n × 首次间隔, 30s)
constexpr qint64 kBackoffCapMs = 30000;
/// 首次重试间隔：2s → 2/4/8/16/30s 这条梯子（见 backoffMs）
constexpr int kRetryBaseMs = 2000;
/// 队列空转时调用方的重试点（写缓冲水位还没退下来，20ms 后再试一次）
constexpr int kFlushWaitMs = 20;
}

namespace uplink {

/**
 * @brief 一条报警的上行视图（AlarmRecord 的字段子集）
 *
 * 不直接收 AlarmRecord：那个头会牵进 common.hpp（OpenCV/RKNN）与 DAO 链接链，
 * 离线自检就得把整个报警子系统拖进测试目标。AlarmUplink 负责逐字段搬运。
 */
struct AlarmInfoInput {
    QString alarmId;             ///< AlarmRecord::id（32 位十六进制）
    QString alarmType;           ///< 报警类型键，如 un_helmet / fence
    int     alarmLevel = 3;      ///< 1 紧急 2 重要 3 一般 4 提示
    QString alarmTime;           ///< 报警时间，原样透传（对端不做时区换算）
    int     channel = 0;         ///< **0-based**（与 DB/内部一致）；上行 JSON 里统一 +1
    int     classId = 0;
    QString className;
    float   confidence = 0.0f;
    bool    isFenceAlarm = false;
    QString status = QStringLiteral("pending");
    QString imagePath;           ///< 抓拍图路径，本批次共享；空 = 没有图
    QString videoPath;           ///< 报警时刻恒为空，片段路径由 videoClipAttached 补
    // 施工信息整份 JSON（AlarmRecord::constructionJson 原样搬）。上行时作为
    // data["construction"] **嵌套子对象**，键名待接口方确认。
    // 空串（未登录/历史行为）⇒ 这个键根本不出现在报文里。
    QString constructionJson;
};

/**
 * @brief alarm_info 里 snapshot / clip 子对象的内容
 *
 * expectedBytes / availableInMs 是**预告值**：<0 表示"还不知道"，字段直接省略；
 * 权威数值只在 media_begin 里给。
 * url 非空 = 本机 http 文件服务上的可下载地址（`transfer:"http"`）。注意它只是"路径已经定了"，
 * 不代表文件已经落盘 —— 抓拍与片段都是异步写的，所以 planned 仍是 true，
 * 对端要在收到对应 media_begin 之后才保证取得到。
 */
struct MediaRef {
    QString mediaId;             ///< 本引擎生成，s-/c- 前缀 + 16 位十六进制
    QString name;                ///< 文件名（clip 在报警时刻只能按命名规则猜，仅提示用）
    QString mime;
    QString url;                 ///< 空 = 此刻给不出地址（url 模式下 clip 就是这种）
    qint64  expectedBytes = -1;
    int     availableInMs = -1;
    bool    planned = true;      ///< true = 此刻文件还没落盘
};

/**
 * @brief 分块计划：一段媒体切成多少个二进制帧
 *
 * Qt 5.12 的 QWebSocket 一次 sendBinaryMessage 就是一次 doWriteFrames，没有分片循环
 * （只有头文件、.so 无符号，出站是否分片属推断），所以块大小必须由应用层定：
 * 单帧落在常见服务器默认上限内，对端上限更小就调小 chunk_size，不改代码。
 */
struct ChunkPlan {
    qint64 totalBytes = 0;
    int chunkSize = 0;
    int totalChunks = 0;      ///< 0 字节 = 0 块（空文件不发媒体，调用方按失败处理）
    int lastChunkBytes = 0;   ///< 末块是余数
};
ChunkPlan planChunks(qint64 totalBytes, int chunkSize);

/// 第 n 次重试（n 从 1 起）要等多久：min(2^n × base, kBackoffCapMs)
qint64 backoffMs(int attempt, int baseMs);

/**
 * @brief 把本机绝对路径换算成 http 文件服务上的地址（url 模式用）
 * @param baseUrl  前缀，形如 `http://192.168.1.50:8090`，末尾带不带斜杠都行
 * @param rootDir  服务的根目录绝对路径（= data/alarms 的绝对化），报警媒体都在它下面按日期分层
 * @param absPath  要暴露的文件**绝对路径**
 * @return 完整地址；**空串 = 这个路径不该给出去**（三个入参任一为空，或 absPath 不在 rootDir 之下）
 *
 * 为什么要"不在 rootDir 之下就返回空"：服务零鉴权，root 之外的东西一旦拼出地址就等于
 * 公开本机任意可读文件。所以这里不做"尽力而为"，越界就是不给。
 * ⚠ absPath 必须调用方自己绝对化：底层 QDir::relativeFilePath 对相对入参是**按 rootDir 解析**
 * 而不是按 cwd，把工程里到处都是的 "data/alarms/…" 直接传进来，会得到一个多套一层前缀的 404 地址。
 * 每段路径都按 QUrl 百分号编码：文件名里的类别名来自模型 labels.txt，中文标签是合法的，
 * 裸拼进去对端拿到的是非法 URL。
 */
QString mediaUrlFor(const QString &baseUrl, const QString &rootDir, const QString &absPath);

/// 一个待发出站帧：文本走 sendText、二进制走 sendBinary，同一条连接上帧有序
struct OutboundFrame {
    bool binary = false;
    QString text;        ///< 文本帧内容（已渲染成紧凑 JSON）
    QByteArray bytes;    ///< 二进制帧内容
};

/// 一段媒体的身份（media_begin 里要写的字段），字节由 MediaPump 在后台线程读好交进来
struct MediaJob {
    QString mediaId;     ///< 与 alarm_info 里预告的那个一致
    QString alarmId;     ///< 归属报警（同批多条时取 slot 的第一条）
    QString kind;        ///< snapshot | clip
    QString name;        ///< **落盘后的真实文件名**，覆盖 alarm_info 里的猜测
    QString mime;        ///< 同上，真实扩展名换算出来的
};

/**
 * @brief submit() 的回执：本条报文的 id，以及"这段媒体该不该去读盘"
 *
 * 为什么调用方需要知道 first：媒体字节只该上传一次，而一帧可以产多条报警行、
 * 共享同一张抓拍图（deliverAlarms）。slot 表里"这次是新建"正是那唯一的一次，
 * 把它透出来，AlarmUplink 就能只在这一次把读盘任务交给 MediaPump。
 */
struct SubmitResult {
    QString messageId;             ///< 空 = 本条被丢弃（缺 alarm_id 的脏数据）
    QString snapshotMediaId;       ///< 空 = 没申报抓拍（配置关了或没有图）
    bool snapshotFirst = false;    ///< true = 抓拍 slot 本次新建 → 该读这张图了
    QString clipMediaId;           ///< 空 = 没申报片段（配置关了或 recording 没开）
};

/**
 * @brief 组装第一条上行报文：外层信封 {type, message_id, ts, data}
 * @param siblingAlarmIds 与本条共享同一份媒体的报警 id（含自身），来自 media slot
 * @param snapshot/clip   nullptr 表示该段不启用（配置关了，或压根没有这个文件）
 * @param transfer        媒体交付方式字面量：kTransferHttp（url 模式）| kTransferChunks（分块帧）
 * @note channel 在这里 +1：内部 0-based、上线 1-based 是本项目最容易对不上的字段
 */
QJsonObject buildAlarmInfoMessage(const AlarmInfoInput &alarm,
                                  const QStringList &siblingAlarmIds,
                                  const MediaRef *snapshot,
                                  const MediaRef *clip,
                                  const QString &messageId,
                                  const QString &ts,
                                  const char *transfer = uplink::kTransferChunks);

/// 媒体开始：把真实字节数、块大小、块数、sha256 一次说清（对端据此校验完整性）
QJsonObject buildMediaBeginMessage(const MediaJob &job,
                                   const ChunkPlan &plan,
                                   const QString &sha256Hex,
                                   const QString &messageId,
                                   const QString &ts);

/// 媒体结束：seqsSent 是**实际发出去的二进制帧数**，与 totalChunks 不等就说明这段不完整
QJsonObject buildMediaEndMessage(const QString &mediaId,
                                 int seqsSent,
                                 int totalChunks,
                                 const QString &sha256Hex,
                                 const QString &messageId,
                                 const QString &ts);

/**
 * @brief url 模式的"媒体可取了"：与 buildMediaBeginMessage 同一个 type 名，只差 transfer/url
 * @note 复用 kTypeMediaBegin 而不是另起一个类型名，是为了让对端只需解析一种媒体消息，
 *       按 transfer 决定"接下来等 N 个二进制帧"还是"照 url 去下载"。
 *       url 模式**没有 media_end**，也没有 chunk_size/total_chunks。
 */
QJsonObject buildMediaBeginUrlMessage(const MediaJob &job,
                                      const QString &url,
                                      qint64 totalBytes,
                                      const QString &sha256Hex,
                                      const QString &messageId,
                                      const QString &ts);

/// 媒体承诺落空：这段媒体的字节永远不会到了，对端按 media_id 撤掉那一条 planned
QJsonObject buildMediaFailedMessage(const QString &mediaId,
                                    const QString &alarmId,
                                    const QString &kind,
                                    const QString &reason,
                                    const QString &messageId,
                                    const QString &ts);

/// 前缀 + 16 位十六进制；对端只当唯一键用，不含语义（a-/s-/c-/m- 都走这里）
QString newMediaId(const char *prefix);

/// 报警上行的报文组装 + 待发队列（详见文件头）
class AlarmUplinkEngine
{
public:
    struct Settings {
        int  maxPending   = 16;   ///< 待发队列上限（信息条与媒体段同用一个池），满了丢最旧
        bool sendSnapshot = true;
        bool sendClip     = true; ///< 上层负责把 recording.enabled=false 折算成 false
        QString clipMime  = QString::fromLatin1(uplink::kMimeHevc);  ///< 由 recording.codec 推
        int  chunkSize    = 262144; ///< 二进制帧大小，qBound 由 ConfigManager 做
        /// 媒体怎么交出去。true = 本机 http 文件服务，JSON 只带地址；
        /// false = 字节走 WS 二进制分块帧。判据由 AlarmUplink::attach() 从 media_transport
        /// 折算一次，引擎里只认这个 bool。
        /// ⚠ **这里的默认与产品配置的默认相反**：引擎默认走 ws_binary，因为它是
        /// 不依赖任何外部服务就能跑通的一档（tests/test_alarm_uplink.cpp 那 11 组基线全建在它上面）。
        /// 产品出厂默认是 url，由 attach() 显式写进来。忘了写不会出错，只会退回旧行为，
        /// 而日志里的 transport= 一行当场就能看出来
        bool mediaViaUrl  = false;
        /// url 模式的地址前缀。**空 = 地址给不出来**（服务没起来 / 没有可用 IPv4 且没配前缀），
        /// 此时两条媒体腿整体不申报，alarm_info 的文本照发
        QString mediaBaseUrl;
        /// 文件服务根目录（绝对路径），mediaUrlFor 的换算基准
        QString mediaRootDir;
        /// 要求对端回 uplink_ack。**默认 false**：Pad 现在根本不回这个类型，
        /// 当成必经状态会让队列被永远等不到的确认堵住
        bool expectAck    = false;
        int  ackTimeoutMs = 15000; ///< 等到这个点还没确认就算一次失败重试
        int  maxAttempts  = 3;     ///< 重发这么多次还没有确认就判失败（丢项）
    };
    using Logger = std::function<void(const QString &category, const QString &message)>;

    void setTransport(UplinkTransport *transport) { transport_ = transport; }  ///< 不拥有
    void setSettings(const Settings &settings) { settings_ = settings; }
    void setLogger(Logger logger) { logger_ = std::move(logger); }

    /**
     * @brief 收到一条报警：登记媒体 slot，组好 alarm_info 入队
     * @return 回执，见 SubmitResult；alarmId 为空则 messageId 为空并丢弃（DB 主键都没有的脏数据）
     * @note 只入队不发送——发送由 pumpOut() 驱动，保证水位闸门只有一处判据
     */
    SubmitResult submit(const AlarmInfoInput &alarm);

    /**
     * @brief 按报警 id 反查它在 alarm_info 里承诺过的片段 media_id
     * @return 第一个命中的 media_id；空 = 这批报警压根没承诺过片段
     * @note 片段的落盘路径与报警时刻之间只共享 id（videoClipAttached 只给 ids 和路径），
     *       而 media_id 是引擎生成的，所以这张反查表只能放在引擎里
     */
    QString clipMediaIdFor(const QStringList &alarmIds) const;

    /// 片段已交给读盘链路（或确定不会有了）之后清掉承诺，免得反查表只增不减
    void forgetClipPromises(const QStringList &alarmIds);

    /**
     * @brief 后台读盘完成后提交一段媒体：begin + N 个二进制帧 + end 入队
     * @param payload  整个文件的字节（切块在此处发生，1.2MB 量级，见类注释的内存预期）
     * @param sha256Hex 由 MediaPump 算好（哈希不能上 GUI 线程）
     * @return 本段的 media_id；空载荷返回空串并记 warning（空文件没什么可上传）
     */
    QString submitMedia(const MediaJob &job, const QByteArray &payload, const QString &sha256Hex);

    /**
     * @brief url 模式的第二段：一段媒体已经落盘并写稳，把它的地址发出去
     * @param sizeBytes 真实字节数（MediaPump 等写稳时采到的最后一个 size）
     * @param sha256Hex 该文件哈希，让对端下载完能自校验；算不出来传空串则省略字段
     * @return 本段的 media_id；空 url（地址给不出来）返回空串并记 warning
     *
     * 与 submitMedia 的关键差别：这里入队的项 `media=false`、只有**一个文本帧**。
     * 标成 true 会被 pumpOut() 的"游标越界就补 media_end"逻辑凭空造出一条
     * `media_end{seqs_sent:0,total_chunks:0}`，还会让 restartInFlightMedia() 每次重连
     * 都重复申报一遍。url 模式没有帧序列要收尾，media_end 整个不发。
     */
    QString submitMediaUrl(const MediaJob &job, const QString &url,
                           qint64 sizeBytes, const QString &sha256Hex);

    /**
     * @brief 一段在 alarm_info 里承诺过的媒体**确定不会有了**，通知对端别再等
     * @param reason 只写人机可读的原因，绝不含文件内容
     *
     * 三个来源：MediaPump 读盘失败/被挤掉、ClipWriter 丢最旧、dump 裸流或 mp4 副本失败。
     * 这三种过去只打本机日志，对端手里就攥着一条永远兑现不了的 planned —— url 模式下更糟，
     * 对端会真的去取一个 404。两档 transport 都该发，所以放在引擎里而不是调用方。
     */
    void submitMediaFailed(const QString &mediaId, const QString &alarmId,
                           const QString &kind, const QString &reason);

    /**
     * @brief 按发送水位把队列里的帧发出去
     * @return 本轮**完整发完**的项数（半截的媒体项不计入，但它的帧已部分发出）
     * @note 严格顺序：一帧失败（返回 <=0）就停在本轮，后面的不许插队
     */
    int pumpOut();

    /// 重连后调用：正在进行的媒体段从**第一块**重发（不做断点续传，半截流对端没法用），
    /// 靠 media_begin 的 sha256 让对端去重
    void restartInFlightMedia();

    /**
     * @brief 收到对端 uplink_ack 的 data 字段
     * @param phase "info" | "media"；决定用 messageId 还是 mediaId 认领队首
     * @param status ok | retry | reject
     * @note 只与队首对账：发送是严格顺序的，队首之外不可能有别的项在等确认。
     *       id 对不上就当噪声记一条 warning（对端实现写错时，这条日志是唯一线索）
     */
    void handleAck(const QString &messageId, const QString &mediaId,
                   const QString &phase, const QString &status, const QString &error);

    /// 调用方下一次该在多少毫秒后再泵一次：等确认/退避中就等到那个点，
    /// 否则（纯水位没退下来）用 kFlushWaitMs；队列空返回 0（调用方应停表）
    int nextWaitMs() const;

    int pendingCount() const { return static_cast<int>(queue_.size()); }
    bool hasPending() const { return !queue_.empty(); }
    void clear();

private:
    struct MediaSlot {
        QString mediaId;
        QStringList alarmIds;
    };

    /// 命中则复用并追加 alarmId；未命中则新建 media_id 入 slot 表（超上限淘汰最早登记）
    /// 按值返回：QHash 插入会让已有引用失效，这里不返回引用就是为了不给调用留悬垂的口子
    /// fresh（可空）带出"本次是新建"，见 SubmitResult::snapshotFirst 的理由
    MediaSlot touchSlot(const QString &kind, const QString &batchKey, const QString &alarmId,
                        bool *fresh = nullptr);

    /// 队首已满时挤掉最旧的一项并记日志（与 SnapWriter/ClipWriter 同一口径）
    void trimQueueToFit();

    /// 记一条"这条报警会有一个片段"，超过 kMaxClipPromises 挤掉最早那条
    void rememberClipPromise(const QString &alarmId, const QString &mediaId);

    /**
     * @brief 队首按 ack_timeout/max_attempts 走一次失败处理（只作用于队首）
     * @return true = 已置退避时刻，稍后整项重发；false = 已判失败并出队
     */
    bool retryFront(const QString &why);

    void logMsg(const char *category, const QString &message) const;
    UplinkTransport *transport_ = nullptr;
    Settings settings_;
    Logger logger_;

    /// 待发项的种类，只用于日志与"丢了它对端手里会不会剩一条空头承诺"的判断
    enum class ItemKind {
        AlarmInfo,     ///< 一条 alarm_info（1 个文本帧）
        MediaChunks,   ///< 一段媒体的 begin + N 个二进制块（end 发时现组），media=true
        MediaUrl,      ///< 一段媒体的 http 地址（1 个文本帧），media=false
        MediaFailed,   ///< 一段媒体的承诺落空（1 个文本帧），media=false
    };

    /// 一个待发项：一条 alarm_info（1 帧）或一段媒体（begin + N 个二进制块，end 发时现组）
    struct PendingItem {
        ItemKind kind = ItemKind::AlarmInfo;
        QString itemId;              ///< 信息项 = message_id；媒体项 = media_id
        QString alarmId;             ///< 日志与归属用
        bool media = false;
        QVector<OutboundFrame> frames;
        int cursor = 0;              ///< 下一个要发的帧
        int binarySent = 0;          ///< 实际发出的二进制帧数，写进 media_end.seqs_sent
        int totalChunks = 0;         ///< 计划块数，与 binarySent 不等就是对端该怀疑
        QString sha256;              ///< 媒体项：组 media_end 要用
        bool failLogged = false;     ///< 同一项的发送失败只报第一次，免得刷屏
        int attempts = 0;            ///< 已重发次数（expect_ack 打开时才有意义）
        bool awaitingAck = false;    ///< 帧已全发出去，等对端确认
        qint64 ackDeadlineMs = 0;    ///< 过点即算一次失败重试
        qint64 retryAfterMs = 0;     ///< 退避中：到这个时刻才允许再发
    };
    std::deque<PendingItem> queue_;

    QHash<QString, MediaSlot> slots_;
    std::deque<QString> slotOrder_;   ///< slot 的登记顺序，淘汰用（QHash 本身无序）

    /// alarm_id → 承诺过的片段 media_id（见 clipMediaIdFor）
    QHash<QString, QString> clipPromises_;
    std::deque<QString> promiseOrder_;   ///< 同样按登记顺序封顶淘汰
};

}  // namespace uplink

#endif  // ALARM_UPLINK_ENGINE_H
