#include "alarm_manager.h"
#include "ConfigManager.h"
#include <QDateTime>
#include <QDebug>

AlarmManager &AlarmManager::instance()
{
    static AlarmManager mgr;
    return mgr;
}

AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
{
    // 从 config.json 读取报警类别
    alarmClasses_ = ConfigManager::instance().alarmClasses();
    if (alarmClasses_.isEmpty()) {
        alarmClasses_ << "person";
    }
}

void AlarmManager::setClassNames(const QStringList &names)
{
    QMutexLocker lock(&mutex_);
    classNames_ = names;
}

void AlarmManager::onDetectionResult(int channel, const object_detect_result_list &results)
{
    QVector<AlarmRecord> newAlarms;
    {
        QMutexLocker lock(&mutex_);
        totalDetections_ += results.count;

        for (int i = 0; i < results.count; i++) {
            const object_detect_result &det = results.results[i];

            QString className;
            if (det.cls_id >= 0 && det.cls_id < classNames_.size()) {
                className = classNames_[det.cls_id];
            } else {
                className = QString("cls_%1").arg(det.cls_id);
            }

            classCount_[className]++;

            // 检查是否需要生成报警
            if (alarmClasses_.contains(className)) {
                // 去重限流：同通道同类别在限流窗口内只报一次
                QString key = QString("%1:%2").arg(channel).arg(det.cls_id);
                long last = lastAlarmTime_.value(key, 0);
                if (results.time - last < kAlarmThrottleNs) {
                    continue;
                }
                lastAlarmTime_[key] = results.time;

                AlarmRecord alarm;
                alarm.timestamp = results.time;
                alarm.channel = channel;
                alarm.clsId = det.cls_id;
                alarm.className = className;
                alarm.confidence = det.prop;
                alarm.acknowledged = false;
                alarms_.append(alarm);
                newAlarms.append(alarm);

                // 限制报警数量，最多保留 1000 条
                if (alarms_.size() > 1000) {
                    alarms_.removeFirst();
                }
            }
        }
    } // 解锁后再发信号，避免死锁

    for (const AlarmRecord &alarm : newAlarms)
        emit alarmGenerated(alarm);
    emit statsUpdated();
}

void AlarmManager::setChannelOnline(int channel, bool online)
{
    QMutexLocker lock(&mutex_);
    if (online)
        onlineChannels_.insert(channel);
    else
        onlineChannels_.remove(channel);
}

int AlarmManager::onlineChannelCount() const
{
    QMutexLocker lock(&mutex_);
    return onlineChannels_.size();
}

QVector<AlarmRecord> AlarmManager::alarms() const
{
    QMutexLocker lock(&mutex_);
    return alarms_;
}

int AlarmManager::alarmCount() const
{
    QMutexLocker lock(&mutex_);
    return alarms_.size();
}

int AlarmManager::unacknowledgedCount() const
{
    QMutexLocker lock(&mutex_);
    int count = 0;
    for (const auto &a : alarms_) {
        if (!a.acknowledged) count++;
    }
    return count;
}

void AlarmManager::acknowledgeAll()
{
    QMutexLocker lock(&mutex_);
    for (auto &a : alarms_) {
        a.acknowledged = true;
    }
}

void AlarmManager::clearAlarms()
{
    QMutexLocker lock(&mutex_);
    alarms_.clear();
}

QMap<QString, int> AlarmManager::classStatistics() const
{
    QMutexLocker lock(&mutex_);
    return classCount_;
}

int AlarmManager::totalDetections() const
{
    QMutexLocker lock(&mutex_);
    return totalDetections_;
}

void AlarmManager::resetDailyStats()
{
    QMutexLocker lock(&mutex_);
    totalDetections_ = 0;
    classCount_.clear();
}

void AlarmManager::setAlarmClasses(const QStringList &classes)
{
    QMutexLocker lock(&mutex_);
    alarmClasses_ = classes;
}

QStringList AlarmManager::alarmClasses() const
{
    QMutexLocker lock(&mutex_);
    return alarmClasses_;
}
