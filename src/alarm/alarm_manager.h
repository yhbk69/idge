#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QMutex>
#include <QMap>
#include <QStringList>
#include <QSet>
#include <atomic>
#include "common.hpp"

struct AlarmRecord {
    long timestamp;
    int channel;
    int clsId;
    QString className;
    float confidence;
    bool acknowledged;
    QString imgPath;      ///< 报警截图路径（可为空）
};

Q_DECLARE_METATYPE(AlarmRecord)

class AlarmManager : public QObject
{
    Q_OBJECT
public:
    static AlarmManager &instance();

    // 同通道同类别去重限流间隔（纳秒），默认 2 秒
    static const long kAlarmThrottleNs = 2000LL * 1000000LL;

    void setClassNames(const QStringList &names);

    // 在产生画面的线程(解码线程)调用：统计 + 判定报警（含去重限流）
    // 返回本次真正需要上报的新报警（尚未存储，imgPath 待填充）
    QVector<AlarmRecord> ingest(int channel, const object_detect_result_list &results);

    // 存储新报警并发出 alarmGenerated / statsUpdated 信号
    void storeAndNotify(const QVector<AlarmRecord> &alarms);

    QVector<AlarmRecord> alarms() const;
    int alarmCount() const;
    int unacknowledgedCount() const;
    void acknowledgeAll();
    void clearAlarms();

    QMap<QString, int> classStatistics() const;
    int totalDetections() const;
    void resetDailyStats();

    // 报警类别配置
    void setAlarmClasses(const QStringList &classes);
    QStringList alarmClasses() const;

    // 通道在线状态
    void setChannelOnline(int channel, bool online);
    int onlineChannelCount() const;

    // 报警时是否截图存档（界面按钮可开关）
    void setScreenshotsEnabled(bool on);
    bool screenshotsEnabled() const;

signals:
    void alarmGenerated(const AlarmRecord &alarm);
    void statsUpdated();

private:
    explicit AlarmManager(QObject *parent = nullptr);

    QVector<AlarmRecord> alarms_;
    mutable QMutex mutex_;
    int totalDetections_ = 0;
    QMap<QString, int> classCount_;
    QStringList classNames_;
    QStringList alarmClasses_;
    QMap<QString, long> lastAlarmTime_;   // key: "ch:cls" -> 上次报警时间(ns)
    QSet<int> onlineChannels_;            // 当前在线通道
    std::atomic<bool> screenshotsOn_{true};  // 报警是否截图
};

#endif // ALARM_MANAGER_H
