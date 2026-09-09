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

// ============================================================================
// AlarmRecord - 报警记录结构体
// ============================================================================
// 存放一条报警的完整信息：
//   - timestamp: 报警时间戳
//   - channel: 视频通道编号
//   - clsId: 检测到的类别 ID
//   - className: 类别名称（如 "person"、"helmet"）
//   - confidence: 置信度（0.0 ~ 1.0）
//   - acknowledged: 是否已确认（用户点击确认后为 true）
//   - imgPath: 报警截图路径（可为空）
//
// ============================================================================
struct AlarmRecord {
    long timestamp;          // 报警时间戳
    int channel;             // 视频通道编号
    int clsId;               // 类别 ID
    QString className;       // 类别名称
    float confidence;        // 置信度
    bool acknowledged;       // 是否已确认
    QString imgPath;         // 报警截图路径（可为空）
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

    // 同通道同类别去重限流间隔（纳秒），默认 2 秒
    // 例如：通道 0 检测到 person 后，2 秒内不会再次报警
    static const long kAlarmThrottleNs = 2000LL * 1000000LL;

    // 设置类别名称列表（用于将 cls_id 转换为类别名称）
    void setClassNames(const QStringList &names);

    // ============================================================================
    // ingest: 接收检测结果，判断是否需要生成报警
    // ============================================================================
    // 在产生画面的线程(解码线程)调用：统计 + 判定报警（含去重限流）
    // 返回本次真正需要上报的新报警（尚未存储，imgPath 待填充）
    //
    // 参数：
    //   - channel: 视频通道编号
    //   - results: 检测结果列表
    //
    // 返回：新生成的报警列表
    // ============================================================================
    QVector<AlarmRecord> ingest(int channel, const object_detect_result_list &results);

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

    // 清空所有报警
    void clearAlarms();

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
    QStringList classNames_;                   // 类别名称列表
    QStringList alarmClasses_;                 // 报警类别列表
    QMap<QString, long> lastAlarmTime_;        // 上次报警时间（用于去重限流）
                                               // key: "通道号:类别ID" -> 上次报警时间(ns)
    QSet<int> onlineChannels_;                 // 当前在线通道集合
    std::atomic<bool> screenshotsOn_{true};    // 报警是否截图（原子变量，线程安全）
};

#endif // ALARM_MANAGER_H
