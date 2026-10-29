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
//   - isFenceAlarm: 是否为电子围栏侵入报警（区别于按模型报警类别判定的检测报警，
//                   走 geofence.throttleSeconds 独立限流窗口）
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
    QString status = "pending";  // 状态: pending/rectified/false_alarm
    QString disposeResult;    // 处置结果
    int disposeUserId = 0;    // 处置人ID
    QString disposeUserName;  // 处置人姓名
    QString disposeTime;      // 处置时间
    QString disposePhoto;     // 现场处置照片路径
    QString remark;           // 补充说明
    QString readTime;         // 已读时间
    QString createTime;       // 入库时间
    QString updateTime;       // 更新时间

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
    // ingestFenceAlarm: 电子围栏侵入报警
    // ============================================================================
    // 由解码线程在"通道级合并围栏判定"里对每个**新进入**围栏的目标调用一次。
    // 与 ingest 的区别：
    //   - 不做类别匹配判断——能调到这里说明调用方已经判定"该目标在围栏内"，
    //     并且已经用 IoU 事件表确认这不是"同一个人还在栏内"（进入沿触发），
    //     围栏类别由 geofence.alarmClass 单值决定，与槽位的 alarm_class 无关；
    //   - 不用 2 秒检测限流窗口，用自己的围栏窗口 geofence.throttleSeconds
    //     （默认 10 秒），限流键 "通道:fence:类别名"（用类名而非槽位，
    //     配置重排后仍稳定）。
    // 语义："一次入侵事件报一条"由调用方保证，本窗口是它之下的兜底——进入沿抖动
    // 或多人几乎同时进入时，每通道每类别窗口内只留第一条。
    //
    // 参数 ctx 取检出该目标的槽位，仅用于把 cls_id 翻成类别名（含 detections 明细）。
    // ============================================================================
    QVector<AlarmRecord> ingestFenceAlarm(int channel, const object_detect_result &det,
                                          const SlotContext &ctx, long timeNs);

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

    // 标记为误报（按ID）- 推荐使用
    bool markAsFalsePositiveById(const QString &id);

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

signals:
    // 报警生成信号（通知界面显示新报警）
    void alarmGenerated(const AlarmRecord &alarm);

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
};

#endif // ALARM_MANAGER_H
