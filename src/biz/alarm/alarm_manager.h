#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

// ============================================================================
// alarm_manager.h - 报警管理器
// ============================================================================
//
// 作用：
//   管理报警的生成、存储、去重和通知。
//   是整个报警系统的核心组件。
//
// 主要职责：
//   1. 接收检测结果，判断是否需要生成报警
//   2. 去重限流：同通道同类目标在限流窗口内只报一次
//   3. 存储报警记录（最多 1000 条）
//   4. 通过 Qt 信号通知界面更新
//
// 使用方式：
//   // 在解码线程中调用（ctx 为该槽位的类名表 + 报警类别，见 SlotContext）
//   QVector<AlarmRecord> newAlarms =
//       AlarmManager::instance().ingest(channel, results, ctx);
//   if (!newAlarms.isEmpty()) {
//       AlarmManager::instance().storeAndNotify(newAlarms);
//   }
//
// ============================================================================

#include <QObject>
#include <QString>
#include <QVector>
#include <QMutex>
#include <QMap>
#include <QStringList>
#include <QSet>
#include <atomic>
#include <deque>
#include "common.hpp"
#include "runtime_paths.h"
#include "fence_shape.h"   // geofence::FenceRole：围栏报警按角色分闯入/离岗

// 前向声明
class QSqlDatabase;

// ============================================================================
// AlarmRecord - 报警记录结构体
// ============================================================================
// 存放一条报警的完整信息：
//   - timestamp: 报警时间戳（直接取自 results.time，与推理结果时间同源，
//                量纲见 common.hpp，注意与限流常量的单位一致性隐患）
//   - channel: 视频通道编号
//   - clsId: 检测到的类别 ID
//   - className: 类别名称（如 "person"、"helmet"）
//   - confidence: 置信度（0.0 ~ 1.0）
//   - acknowledged: 确认状态位——纯内存态，UI 点击"确认"后经 acknowledgeAll()
//                   批量置 true，用于 unacknowledgedCount() 的未读角标；不入库
//   - imgPath: 报警抓拍图路径（可为空）——本管理器只负责判定与产出记录，
//              截图落盘由 SnapWriter 后台线程异步完成并回填，与 SQLite 落库解耦：
//              一条报警 = 一行 DB 记录 + 一张 data/alarms/日期/ 下的抓拍图，二者非同一生命周期
//   - isFenceAlarm: 是否为电子围栏报警（闯入禁区 / 离岗，alarmType 为 fence_in / fence_out；
//                   区别于按模型报警类别判定的检测报警，走 geofence.throttleSeconds 独立限流窗口，
//                   节流键按角色分开，所以闯禁区后 10 秒内的离岗不会被吞）
//
// ============================================================================
struct AlarmRecord {
    QString id;               // UUID 主键
    QString alarmType;        // 报警类型 (no_helmet, fire, fence等)
    int alarmLevel = 3;       // 报警级别: 1紧急 2重要 3一般 4提示
    QString alarmTime;        // 报警发生时间
    int channel;              // 视频通道编号
    int clsId;                // 类别 ID
    QString className;        // 类别名称
    float confidence;         // 置信度
    QString imagePath;        // 报警图片路径
    QString videoPath;        // 报警视频路径
    QString status = "pending";  // 状态: pending/rectified/disposed/false_alarm（disposed 只由 AlarmDAO::dispose() 写，对端即 v1 的 DISPOSED）
    QString disposeResult;    // 处置结果
    int disposeUserId = 0;    // 处置人ID
    QString disposeUserName;  // 处置人姓名
    QString disposePersonId;  // 处置人/误报人工号**原文**（v1 的 dispose.personId，例 "05-0101"）。
                              // 与 disposeUserId 分两列是因为后者是 INTEGER，装不下带横线的工号
    QString disposeTime;      // 处置时间
    QString disposePhoto;     // 处置整改照片：路径的 **JSON 数组文本**（0~5 个相对路径）。
                              // ⚠ 老行可能是单张裸路径，读侧两种形状都要认（见 pad_wire.cpp 的 photoUrls）
    QString remark;           // 补充说明
    QString readTime;         // 已读时间
    QString createTime;       // 入库时间
    QString updateTime;       // 更新时间

    // 施工信息（扫码登录时定死，报警产生那一刻打戳；未登录/历史行为空）
    //   workerName: 列表那一列按它显示，也是将来按人筛选的落点（取自 person.name）
    //   constructionJson: 整份载荷（person/plan 嵌套，含人员与计划时间窗），详情框按 JSON 逐键渲染、
    //                     WebSocket 上行作为 data["construction"] 子对象——
    //                     **字段增删只改 ConstructionInfo，这两列与这两处显示都不用动**
    //   taskId: 这一条属于哪个施工计划 ＝ v1 的 taskId（B2 定案：就是载荷里的 plan.taskId）。
    //           单独成列是为了让 Pad 的 sync 过滤走等值而不是解 JSON；未登录时是空串
    QString workerName;
    QString constructionJson;
    QString taskId;

    // 兼容旧字段
    long timestamp = 0;       // 时间戳(毫秒)
    bool isFenceAlarm = false;
};

// 注册自定义类型（用于 Qt 信号/槽跨线程传递）
Q_DECLARE_METATYPE(AlarmRecord)

// ============================================================================
// AlarmManager - 报警管理器（单例）
// ============================================================================
// 使用单例模式，确保全局只有一个报警管理器实例。
// 线程安全：所有操作都使用 QMutex 保护。
//
// ============================================================================
class AlarmManager : public QObject
{
    Q_OBJECT
public:
    // 获取单例实例
    static AlarmManager &instance();

    // 同通道同类别去重限流间隔（纳秒），名义上 2 秒
    // 例如：通道 0 检测到 person 后，2 秒内不会再次报警
    //
    // 【时钟源（本轮已修复）】旧实现用 results.time（system_clock 墙钟
    //   纳秒）与 lastAlarmTime_ 相减——NTP 回拨后差值变负、恒小于阈值，
    //   报警可被抑制数天；且早期注释声称 time 为"毫秒"（实为纳秒，
    //   该"单位隐患"系文档误差，量纲本就一致）。现改用 steady_clock
    //   单调纳秒参与窗口比较，lastAlarmTime_ 存 steady 纳秒，
    //   与本常量量纲一致且不受墙钟跳变影响。
    // 围栏报警不走这里，它按 geofence.throttleSeconds 配置的独立窗口限流
    // （见 ingestFenceAlarm）。
    static const long kAlarmThrottleNs = 2000LL * 1000000LL;

    /**
     * @brief 接收检测结果并判定报警时，该槽位所需的上下文
     *
     * 由解码器在自己的槽位循环里就地构造（栈上对象，不跨线程共享）。
     * 为什么不放在 AlarmManager 里按槽位下标存表：配置改成按通道之后，
     * 通道 0 的槽位 1 与通道 2 的槽位 1 是两个不同的模型，单例里的同一张表
     * 会被后建任务的通道覆盖，报警类别与类名都会串频道。
     */
    struct SlotContext
    {
        QString     alarmClass;   ///< 该槽位模型的报警类别；空 = 只画框不报警
        QStringList classNames;   ///< 该槽位模型的类别名（labels.txt 行序 = cls_id）
    };

    // ============================================================================
    // ingest: 接收检测结果，判断是否需要生成报警
    // ============================================================================
    // 在产生画面的线程(解码线程)调用：统计 + 判定报警（含去重限流）
    // 返回本次真正需要上报的新报警（尚未存储，imgPath 待填充）
    //
    // 参数：
    //   - channel: 视频通道编号
    //   - results: 检测结果列表
    //   - ctx:     该槽位的类别名表 + 报警类别（见 SlotContext）
    //
    // 返回：新生成的报警列表
    //
    // 只处理"该槽位模型的报警类别"这一种普通检测报警；围栏报警走
    // ingestFenceAlarm（围栏是通道级判定，不属于任何槽位）。
    // ============================================================================
    QVector<AlarmRecord> ingest(int channel, const object_detect_result_list &results,
                                const SlotContext &ctx);

    // ============================================================================
    // ingestFenceAlarm: 电子围栏报警（闯入禁区 / 离岗）
    // ============================================================================
    // 由解码线程在"通道级合并围栏判定"里对每个**新进入违规状态**的目标调用一次。
    // 与 ingest 的区别：
    //   - 不做类别匹配判断——能调到这里说明调用方已经判定该目标违规（禁区里有人，
    //     或人在所有岗区域之外），并且已经用 IoU 事件表确认这不是"同一个人还在
    //     违规状态里"（进入沿触发），围栏类别由 geofence.alarmClass 单值决定，
    //     与槽位的 alarm_class 无关；
    //   - 不用 2 秒检测限流窗口，用自己的围栏窗口 geofence.throttleSeconds
    //     （默认 10 秒），限流键 "通道:fence:角色:类别名"（用类名而非槽位，
    //     配置重排后仍稳定）。
    // 语义："一次事件报一条"由调用方保证，本窗口是它之下的兜底——进入沿抖动
    // 或多人几乎同时违规时，每通道每角色每类别窗口内只留第一条。
    //
    // role 决定两件事，二者都必须分角色，否则两种报警会互相吞：
    //   - alarmType：Forbidden → "fence_in"（闯入禁区）；Workzone → "fence_out"（离岗）
    //     下游按 alarmType 前缀识别围栏报警（见 AlarmDAO::recordFromQuery 的 isFenceAlarm）；
    //   - 限流键里的角色段：不然"先闯入再离岗"的第二次会被第一条的窗口吃掉。
    //
    // 参数 ctx 取检出该目标的槽位，仅用于把 cls_id 翻成类别名（含 detections 明细）。
    // ============================================================================
    QVector<AlarmRecord> ingestFenceAlarm(int channel, const object_detect_result &det,
                                          const SlotContext &ctx, long timeNs,
                                          geofence::FenceRole role);

    // ============================================================================
    // storeAndNotify: 存储新报警并发出信号
    // ============================================================================
    // 存储新报警并发出 alarmGenerated / statsUpdated 信号
    // 信号会通知界面更新报警列表和统计数据
    // ============================================================================
    void storeAndNotify(const QVector<AlarmRecord> &alarms);

    // ============================================================================
    // attachVideoClip: 报警视频片段落盘后回填 videoPath
    // ============================================================================
    // 由 ClipWriter 后台线程调用：片段是"报警后 2 秒"才生成的，报警入库时
    // 文件还不存在，所以路径只能在写好后补。ids 为同一批报警（同通道同瞬间
    // 共享一个片段）。内存与 DB 都更新，末尾发 videoClipAttached（报警上行的第二段）。
    //
    // webPath 是同名的 .mp4 副本，**只用于转发给上行订阅方**：DB 与内存里的 video_path
    // 仍是 Annex-B 裸流。改那个字段会让报警记录与本地图像的关联对不上（界面也读它）。
    // ============================================================================
    void attachVideoClip(const QStringList &alarmIds, const QString &videoPath,
                         const QString &webPath = QString());

    // ============================================================================
    // reportVideoClipSkipped: 告诉订阅者"这批报警不会有片段了"
    // ============================================================================
    // ClipWriter 丢最旧 / recorder 未就绪 / dump 失败时由后台线程调用。上面那个
    // 信号只有成功路径，缺了失败路径订阅方就只能自己猜（详见 videoClipSkipped）。
    // ============================================================================
    void reportVideoClipSkipped(const QStringList &alarmIds, const QString &reason);

    // 获取所有报警记录
    QVector<AlarmRecord> alarms() const;

    // 获取报警总数
    int alarmCount() const;

    // 获取未确认的报警数量
    int unacknowledgedCount() const;

    // 确认所有报警（将所有报警标记为已确认）
    void acknowledgeAll();

    // 确认单条报警（按索引，标记为已确认）
    bool acknowledgeAlarm(int index);

    // 确认单条报警（按ID，标记为已确认）- 推荐使用
    bool acknowledgeAlarmById(const QString &id);

    // 清空所有报警
    void clearAlarms();

    // 删除单条报警（按索引）
    bool removeAlarm(int index);

    // 删除单条报警（按ID）- 推荐使用
    bool removeAlarmById(const QString &id);

    // 标记为误报（按索引）
    bool markAsFalsePositive(int index);

    /**
     * @brief 标记为误报（按ID）- 推荐使用；v1 §4.3 走带后三个参数的那一档
     * @param personId/personName/markedAtInternal 三个都传空串时**与改动前的行为相同**
     *        （本机界面的「误报」按钮就是这样调的）。markedAtInternal 是库内那套
     *        定宽无偏移文本，由 HTTP 路由先把对端的 ISO-8601 换算好再传进来。
     * @return false = id 空 / 库里没这条 / 已是终态（disposed 或已误报，见 §3.7 不回退）
     */
    bool markAsFalsePositiveById(const QString &id, const QString &personId = QString(),
                                 const QString &personName = QString(),
                                 const QString &markedAtInternal = QString());

    /**
     * @brief v1 §4.2 的处置入口（Pad 提交处置表单，含 0~5 张整改照片）
     * @param photoPathsJson 已落盘的整改照片**相对路径**的 JSON 数组文本；空串按 `[]` 存
     * @param disposedAtInternal 对端报的处置时间（库内定宽文本），空 = 本机此刻
     * @param err 可选出参，false 时的中文原因（"库里没有这条报警" / "已经是终态"）
     * @return true = 库里的行真的被改成 disposed 了
     *
     * 为什么必须走这里而不是让 HTTP 路由直接调 AlarmDAO::dispose()：
     *   内存窗口 alarms_ 里那 1000 条是**同一批记录的另一个副本**，
     *   关机时 syncToDatabase() 会把它们整批 INSERT OR REPLACE 回去。
     *   只改库不改这份副本，处置就在第二天早上被静默撤销
     *   ——与 task_id 回填被打回 NULL 是同一个病灶（实测过一次，见 db/README.md）。
     * 成功后照发 alarmStatusChanged，Pad 那边由 WS 收到 alarm_update。
     */
    bool disposeAlarmById(const QString &id, const QString &measure, const QString &remark,
                          const QString &personId, const QString &personName,
                          const QString &photoPathsJson, const QString &disposedAtInternal,
                          QString *err = nullptr);

    /**
     * @brief v1 §4.4 的已读入口：**不改状态**，只推 read_time（保留最早）与 update_time
     * @param readAtInternal 对端报的已读时间（库内定宽文本），空 = 本机此刻
     *
     * 与 acknowledgeAlarmById 的区别要说清：那条是本机「我看过了」，会把状态写成
     * `rectified`；这条只是记录"什么时候被打开过"，状态一动不动
     * （§4.4 明写"读一条报警不改状态"）。同一条报警两者可以都有。
     */
    bool markAlarmReadById(const QString &id, const QString &readAtInternal);

    // 获取各类别检测统计
    QMap<QString, int> classStatistics() const;

    // 获取总检测次数
    int totalDetections() const;

    // 重置每日统计
    void resetDailyStats();

    // ============================================================================
    // 通道在线状态
    // ============================================================================
    // 跟踪哪些通道在线，用于界面显示通道状态
    // ============================================================================
    void setChannelOnline(int channel, bool online);

    /**
     * @brief 这一路现在在线吗（§6.1 的 stream.status 与 §7.1 的 cameras[].online 用它）
     * @note 判据与 onlineChannelCount() 同一份表（onlineChannels_），所以两处不会读出不同的数。
     *       没人调过 setChannelOnline 的通道就是"不在线"——那是事实，不是错误。
     */
    bool isChannelOnline(int channel) const;
    int onlineChannelCount() const;

    // ============================================================================
    // 报警截图开关
    // ============================================================================
    // 报警时是否截图存档（界面按钮可开关）
    // 截图由 SnapWriter 后台线程异步完成，不阻塞解码线程
    // ============================================================================
    void setScreenshotsEnabled(bool on);
    bool screenshotsEnabled() const;

    // ============================================================================
    // 数据库支持
    // ============================================================================

    // 初始化数据库连接
    bool initDatabase(const QString &dbPath = RuntimePaths::database());

    // 数据库是否已初始化
    bool isDatabaseInitialized() const { return dbInitialized_; }

    // 从数据库加载历史报警（程序启动时调用）
    void loadAlarmsFromDatabase(int limit = 1000);

    // 同步内存报警到数据库（程序退出时调用）
    void syncToDatabase();

    // 清理N天前的数据库数据
    void cleanOldData(int daysToKeep = 30);

    // 清空去重限流的内存窗口（检测 2 秒窗口与围栏窗口共用同一张表）。
    // 退出登录时必须调：限流键是"通道:类别"，与施工信息无关，不清的话
    // 重新扫码登录后**第一个窗口期内**的报警会被静默吞掉，界面一条都不出。
    // 不动 DB 历史、不动报警列表本身。
    void resetThrottles();

signals:
    // 报警生成信号（通知界面显示新报警）
    void alarmGenerated(const AlarmRecord &alarm);

    /**
     * @brief 报警**状态**变更（确认 / 误报）信号，携带变更后的完整记录
     *
     * 为什么不能只靠 statsUpdated：后者只说"数字变了"，带不出是哪一条、变成了什么。
     * Pad 接口 v1 §3.7 要的是"这条报警的 status/readAt 变了"（对端按 alarmId 就地合并），
     * 少了这条信号，板端点完「已确认」Pad 那条会一直停在 PENDING，
     * 只能等下次重连靠 sync_result 拉平（报告 P1-1 的实际表现）。
     * 发出时机：状态**真的写成功了**（内存窗口命中或库更新成功）之后。
     */
    void alarmStatusChanged(const AlarmRecord &alarm);

    // 统计更新信号（通知界面刷新统计数据）
    void statsUpdated();

    /**
     * @brief 报警视频片段已落盘（**ClipWriter 后台线程发出**，跨线程接收方必须 QueuedConnection）
     *
     * 比 alarmGenerated 晚约 (post 窗口 + 排队) 毫秒：报警那一刻片段还没写出来。
     * 报警上行用它把 alarm_info 里预告的那个 clip media_id 对上真实路径。
     * @param alarmIds 本片段对应的一批报警 id（同通道同瞬间共享一个片段）
     * @param videoPath Annex-B 裸流路径（DB 里存的那个）
     * @param webPath 同名 .mp4 副本；空 = 副本没写成，url 模式下等于这条报警没有视频
     */
    void videoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                           const QString &webPath);

    /// 片段确定不会产出（队列满丢最旧 / recorder 未就绪 / dump 失败）——同上，后台线程发出
    /// 没有这条通知，"承诺了片段却没有片段"就只是对端单方面的等待
    void videoClipSkipped(const QStringList &alarmIds, const QString &reason);

private:
    // 私有构造函数（单例模式）
    explicit AlarmManager(QObject *parent = nullptr);

    std::deque<AlarmRecord> alarms_;           // 报警记录列表（deque: O(1) pop_front）
    mutable QMutex mutex_;                     // 互斥锁（保护并发访问）
    int totalDetections_ = 0;                  // 总检测次数
    QMap<QString, int> classCount_;            // 各类别检测次数
    QMap<QString, long> lastAlarmTime_;        // 上次报警时间（steady 单调纳秒，仅内存态，用于去重限流）
                                               // key: "通道号:类别ID" -> 上次报警时间(ns)
    QSet<int> onlineChannels_;                 // 当前在线通道集合
    std::atomic<bool> screenshotsOn_{true};    // 报警是否截图（原子变量，线程安全）
    std::atomic<bool> dbInitialized_{false};   // 数据库是否已初始化（原子变量，线程安全）

    /**
     * @brief 取某条报警的**当前**记录（内存窗口命中优先，否则回库读一次）
     *
     * 给 alarmStatusChanged 用：状态变更信号要带完整记录，而对端合并时只看字段值。
     * 找不到（既不在内存窗口也读不到库）返回 id 为空串的默认记录，调用方据此不发。
     */
    AlarmRecord recordForId(const QString &id);
};

#endif // ALARM_MANAGER_H
