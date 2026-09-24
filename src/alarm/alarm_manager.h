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
//   // 在解码线程中调用
//   QVector<AlarmRecord> newAlarms = AlarmManager::instance().ingest(channel, results);
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
#include "common.hpp"

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
//              一条报警 = 一行 DB 记录 + 一张 alarms/日期/ 下的抓拍图，二者非同一生命周期
//   - isFenceAlarm: 是否为电子围栏侵入报警（区别于安全帽/PPE 类报警，通常限流被绕过）
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
    // ⚠ bypassThrottle 形参当前所有调用方均传 false，围栏报警实际
    //   同样受本窗口限流（"围栏不限流"的说法与实现不符）。
    static const long kAlarmThrottleNs = 2000LL * 1000000LL;

    // 设置类别名称列表（用于将 cls_id 转换为类别名称）
    void setClassNames(const QStringList &names);

    // 获取类别名称列表
    const QStringList &classNames() const { return classNames_; }

    /**
     * @brief 设置某个级联槽位的专属类别名列表
     * @param slot  槽位索引（= TaskConfig.result_id = object_detect_result_list.id，0 基）
     * @param names 该槽位模型标签文件读出的类别名，行序 = cls_id
     *
     * 级联各模型类别不同（coco80 / helmet 2 类 / vest 2 类…），
     * ingest 按 results.id 选表，避免 helmet 的 cls_id 被套用 coco 类名。
     */
    void setSlotClassNames(int slot, const QStringList &names);

    /**
     * @brief 按槽位解析类别名
     * @param slot  槽位索引（results.id），越界或未设置时回退全局 classNames_
     * @param clsId 模型输出的类别 ID
     * @return 类别名；两处表都命中不了时返回 "cls_N"
     */
    QString resolveClassName(int slot, int clsId) const;

    // ============================================================================
    // ingest: 接收检测结果，判断是否需要生成报警
    // ============================================================================
    // 在产生画面的线程(解码线程)调用：统计 + 判定报警（含去重限流）
    // 返回本次真正需要上报的新报警（尚未存储，imgPath 待填充）
    //
    // 参数：
    //   - channel: 视频通道编号
    //   - results: 检测结果列表
    //   - bypassThrottle: 是否跳过去重限流（围栏报警不限流）
    //
    // 返回：新生成的报警列表
    // ============================================================================
    QVector<AlarmRecord> ingest(int channel, const object_detect_result_list &results,
                                bool bypassThrottle = false);

    // ============================================================================
    // storeAndNotify: 存储新报警并发出信号
    // ============================================================================
    // 存储新报警并发出 alarmGenerated / statsUpdated 信号
    // 信号会通知界面更新报警列表和统计数据
    // ============================================================================
    void storeAndNotify(const QVector<AlarmRecord> &alarms);

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
    // 报警类别配置
    // ============================================================================
    // 只有在报警类别列表中的类别才会触发报警
    // 例如：配置为 ["person", "helmet"]，则只有检测到人或安全帽时才报警
    // ============================================================================
    void setAlarmClasses(const QStringList &classes);
    QStringList alarmClasses() const;

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
    bool initDatabase(const QString &dbPath = "idge.db");

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

private:
    // 私有构造函数（单例模式）
    explicit AlarmManager(QObject *parent = nullptr);

    QVector<AlarmRecord> alarms_;              // 报警记录列表
    mutable QMutex mutex_;                     // 互斥锁（保护并发访问）
    int totalDetections_ = 0;                  // 总检测次数
    QMap<QString, int> classCount_;            // 各类别检测次数
    QStringList classNames_;                   // 类别名称列表（全局兜底）
    QVector<QStringList> slotClassNames_;      // 按级联槽位存的类别名表，下标=results.id
    QStringList alarmClasses_;                 // 报警类别列表
    QMap<QString, long> lastAlarmTime_;        // 上次报警时间（steady 单调纳秒，仅内存态，用于去重限流）
                                               // key: "通道号:类别ID" -> 上次报警时间(ns)
    QSet<int> onlineChannels_;                 // 当前在线通道集合
    std::atomic<bool> screenshotsOn_{true};    // 报警是否截图（原子变量，线程安全）
    std::atomic<bool> dbInitialized_{false};   // 数据库是否已初始化（原子变量，线程安全）
};

#endif // ALARM_MANAGER_H
