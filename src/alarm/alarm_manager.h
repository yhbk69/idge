#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QMutex>
#include <QMap>
#include <QStringList>
#include <QSet>
#include "common.hpp"

struct AlarmRecord {
    long timestamp;
    int channel;
    int clsId;
    QString className;
    float confidence;
    bool acknowledged;
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

    // 接收检测结果，生成报警
    void onDetectionResult(int channel, const object_detect_result_list &results);

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
};

#endif // ALARM_MANAGER_H
