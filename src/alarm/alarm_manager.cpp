// ============================================================================
// alarm_manager.cpp - 报警管理器实现
// ============================================================================
//
// 作用：
//   管理报警的生成、存储、去重和通知。
//
// 核心功能：
//   1. 接收检测结果，判断是否需要生成报警
//   2. 去重限流：同通道同类目标在限流窗口内只报一次
//   3. 存储报警记录（最多 1000 条）
//   4. 通过 Qt 信号通知界面更新
//
// 报警生成流程：
//   1. ingest(): 接收检测结果
//   2. 检查检测到的类别是否在报警类别列表中
//   3. 检查是否在限流窗口内（默认 2 秒）
//   4. 如果需要报警，创建 AlarmRecord
//   5. storeAndNotify(): 存储报警记录并通知界面
//
// ============================================================================

#include "alarm_manager.h"
#include "ConfigManager.h"
#include <QDateTime>
#include <QDebug>

// 获取单例实例
// 使用静态局部变量实现线程安全的单例
AlarmManager &AlarmManager::instance()
{
    static AlarmManager mgr;
    // 注册自定义类型（用于 Qt 信号/槽跨线程传递）
    static const bool registered = []() {
        qRegisterMetaType<AlarmRecord>("AlarmRecord");
        return true;
    }();
    (void)registered;
    return mgr;
}

// ============================================================================
// 构造函数
// ============================================================================
// 从 config.json 读取报警类别配置
// 默认报警类别：person（检测到人时报警）
// ============================================================================
AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
{
    // 从 config.json 读取报警类别
    alarmClasses_ = ConfigManager::instance().alarmClasses();
    if (alarmClasses_.isEmpty()) {
        alarmClasses_ << "person";  // 默认检测到人时报警
    }
}

// 设置类别名称列表（用于将 cls_id 转换为类别名称）
void AlarmManager::setClassNames(const QStringList &names)
{
    QMutexLocker lock(&mutex_);
    classNames_ = names;
}

// ============================================================================
// ingest: 接收检测结果，判断是否需要生成报警
// ============================================================================
// 参数：
//   - channel: 视频通道编号（0, 1, 2, ...）
//   - results: 检测结果列表
//
// 返回：新生成的报警列表
//
// 处理流程：
//   1. 遍历所有检测结果
//   2. 将 cls_id 转换为类别名称
//   3. 统计各类别检测次数
//   4. 检查是否需要生成报警：
//      a. 类别在报警类别列表中
//      b. 不在限流窗口内（同通道同类 2 秒只报 1 次）
//   5. 创建 AlarmRecord 并添加到新报警列表
//
// ============================================================================
QVector<AlarmRecord> AlarmManager::ingest(int channel, const object_detect_result_list &results)
{
    QVector<AlarmRecord> newAlarms;
    {
        QMutexLocker lock(&mutex_);
        totalDetections_ += results.count;

        for (int i = 0; i < results.count; i++) {
            const object_detect_result &det = results.results[i];

            // 将 cls_id 转换为类别名称
            QString className;
            if (det.cls_id >= 0 && det.cls_id < classNames_.size()) {
                className = classNames_[det.cls_id];
            } else {
                className = QString("cls_%1").arg(det.cls_id);
            }

            // 统计各类别检测次数
            classCount_[className]++;

            // 检查是否需要生成报警
            if (alarmClasses_.contains(className)) {
                // 去重限流：同通道同类别在限流窗口内只报一次
                // key 格式: "通道号:类别ID"，如 "0:0" 表示通道 0 的 person
                QString key = QString("%1:%2").arg(channel).arg(det.cls_id);
                long last = lastAlarmTime_.value(key, 0);
                // 检查是否在限流窗口内（2 秒）
                if (results.time - last < kAlarmThrottleNs) {
                    continue;  // 在限流窗口内，跳过
                }
                lastAlarmTime_[key] = results.time;  // 更新最后报警时间

                // 创建报警记录
                AlarmRecord alarm;
                alarm.timestamp = results.time;
                alarm.channel = channel;
                alarm.clsId = det.cls_id;
                alarm.className = className;
                alarm.confidence = det.prop;
                alarm.acknowledged = false;  // 未确认
                newAlarms.append(alarm);
            }
        }
    }
    return newAlarms;
}

// ============================================================================
// storeAndNotify: 存储报警记录并通知界面
// ============================================================================
// 流程：
//   1. 将新报警添加到报警列表
//   2. 限制报警数量（最多 1000 条，超出时删除最早的）
//   3. 通过 Qt 信号通知界面更新
//
// 注意：
//   - 先解锁，再发信号，避免死锁
//   - 信号/槽在不同线程执行时会自动排队
//
// ============================================================================
void AlarmManager::storeAndNotify(const QVector<AlarmRecord> &alarms)
{
    {
        QMutexLocker lock(&mutex_);
        for (const AlarmRecord &a : alarms) {
            alarms_.append(a);
        }
        // 限制报警数量，最多保留 1000 条
        // 避免内存无限增长
        while (alarms_.size() > 1000) {
            alarms_.removeFirst();  // 删除最早的报警
        }
    } // 解锁后再发信号，避免死锁

    // 通知界面更新
    for (const AlarmRecord &alarm : alarms)
        emit alarmGenerated(alarm);  // 发送报警信号
    emit statsUpdated();  // 发送统计更新信号
}

// 设置通道在线状态
void AlarmManager::setChannelOnline(int channel, bool online)
{
    QMutexLocker lock(&mutex_);
    if (online)
        onlineChannels_.insert(channel);
    else
        onlineChannels_.remove(channel);
}

// 获取在线通道数量
int AlarmManager::onlineChannelCount() const
{
    QMutexLocker lock(&mutex_);
    return onlineChannels_.size();
}

// 设置是否启用截图
void AlarmManager::setScreenshotsEnabled(bool on)
{
    screenshotsOn_.store(on);
}

// 检查是否启用截图
bool AlarmManager::screenshotsEnabled() const
{
    return screenshotsOn_.load();
}

// 获取所有报警记录
QVector<AlarmRecord> AlarmManager::alarms() const
{
    QMutexLocker lock(&mutex_);
    return alarms_;
}

// 获取报警总数
int AlarmManager::alarmCount() const
{
    QMutexLocker lock(&mutex_);
    return alarms_.size();
}

// 获取未确认的报警数量
int AlarmManager::unacknowledgedCount() const
{
    QMutexLocker lock(&mutex_);
    int count = 0;
    for (const auto &a : alarms_) {
        if (!a.acknowledged) count++;
    }
    return count;
}

// 确认所有报警（将所有报警标记为已确认）
void AlarmManager::acknowledgeAll()
{
    QMutexLocker lock(&mutex_);
    for (auto &a : alarms_) {
        a.acknowledged = true;
    }
}

// 清空所有报警
void AlarmManager::clearAlarms()
{
    QMutexLocker lock(&mutex_);
    alarms_.clear();
}

// 获取各类别检测统计
QMap<QString, int> AlarmManager::classStatistics() const
{
    QMutexLocker lock(&mutex_);
    return classCount_;
}

// 获取总检测次数
int AlarmManager::totalDetections() const
{
    QMutexLocker lock(&mutex_);
    return totalDetections_;
}

// 重置每日统计
void AlarmManager::resetDailyStats()
{
    QMutexLocker lock(&mutex_);
    totalDetections_ = 0;
    classCount_.clear();
}

// 设置报警类别列表
void AlarmManager::setAlarmClasses(const QStringList &classes)
{
    QMutexLocker lock(&mutex_);
    alarmClasses_ = classes;
}

// 获取报警类别列表
QStringList AlarmManager::alarmClasses() const
{
    QMutexLocker lock(&mutex_);
    return alarmClasses_;
}
