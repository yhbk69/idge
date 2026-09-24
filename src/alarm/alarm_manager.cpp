// ============================================================================
// alarm_manager.cpp - 报警管理器实现
// ============================================================================
//
// 作用：
//   管理报警的生成、存储、去重和通知。
//   是整个报警系统的核心组件，连接检测层和存储/展示层。
//
// 核心功能：
//   1. ingest(): 接收检测结果，判断是否需要生成报警
//   2. storeAndNotify(): 存储报警到内存+数据库，并通知UI更新
//   3. 去重限流：同通道同类目标在限流窗口内只报一次
//   4. 报警类别过滤：只有配置的类别才触发报警
//
// 报警生成完整流程：
//   ┌─────────────────────────────────────────────────────────────┐
//   │  ffmpeg_video_decoder.cpp::decodeLoop()                    │
//   │    │                                                       │
//   │    ├── 每3帧做一次YOLO检测                                  │
//   │    │                                                       │
//   │    ├── 判断是否有电子围栏                                    │
//   │    │   ├── 有围栏 → 过滤出围栏内目标 → ingest(filtered)      │
//   │    │   └── 无围栏 → 直接 ingest(全部检测结果)                │
//   │    │                                                       │
//   │    ├── AlarmManager::ingest(channel, results)              │
//   │    │   ├── 遍历检测结果                                      │
//   │    │   ├── 检查类别是否在 alarmClasses_ 中                   │
//   │    │   ├── 检查是否在限流窗口内（2秒）                        │
//   │    │   └── 生成 AlarmRecord 列表返回                         │
//   │    │                                                       │
//   │    └── AlarmManager::storeAndNotify(newAlarms)             │
//   │        ├── 添加到内存 alarms_ 列表（最多1000条）              │
//   │        ├── AlarmDAO::insertAlarms() → 写入SQLite数据库       │
//   │        └── emit alarmGenerated() → 通知UI显示报警            │
//   └─────────────────────────────────────────────────────────────┘
//
// ============================================================================

#include "alarm_manager.h"
#include "ConfigManager.h"
#include "database/database_manager.h"
#include "database/alarm_dao.h"
#include "database/detection_dao.h"
#include <QDateTime>
#include <QDebug>
#include <QUuid>
#include <chrono>

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
// 这是报警系统的入口函数，由解码线程调用。
//
// 参数：
//   - channel: 视频通道编号（0, 1, 2, ...）
//   - results: 检测结果列表（包含多个目标的类别、置信度、位置）
//   - bypassThrottle: 是否跳过去重限流（围栏报警默认不限流）
//
// 返回：新生成的报警列表（未存储，由调用方决定何时存储）
//
// 处理流程：
//   1. 加锁保护共享数据（mutex_）
//   2. 遍历所有检测结果：
//      a. 将 cls_id 转换为类别名称（如 "person"、"helmet"）
//      b. 统计各类别检测次数（用于仪表盘展示）
//      c. 检查是否需要生成报警：
//         - 类别必须在 alarmClasses_ 列表中
//         - 同通道同类别必须超过限流间隔（2秒）
//         - bypassThrottle=true 时跳过限流检查
//      d. 创建 AlarmRecord 结构体
//   3. 返回新报警列表
//
// 线程安全：
//   - 整个函数在 mutex_ 保护下执行
//   - 只在解码线程调用，不存在竞争
//
// ============================================================================
QVector<AlarmRecord> AlarmManager::ingest(int channel, const object_detect_result_list &results,
                                         bool bypassThrottle)
{
    QVector<AlarmRecord> newAlarms;
    // 限流窗口计时用单调时钟：results.time 源自 system_clock 墙钟纳秒，
    // NTP 校时/手动改时间发生**回拨**时 `results.time - last` 变负，
    // 恒小于窗口阈值 → 同键报警可被抑制数天。steady_clock 只增不减，
    // 与墙钟彻底解耦（lastAlarmTime_ 因此改存 steady 纳秒，仅内存态）。
    const long steadyNow = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
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
                // 用"通道:类别"作复合键，使不同通道、不同类别各自独立计数互不影响。
                QString key = QString("%1:%2").arg(channel).arg(det.cls_id);
                long last = lastAlarmTime_.value(key, 0);  // 首次为 0，必然放行
                // 与 kAlarmThrottleNs 同为 steady 纳秒，量纲一致；
                // 注意 bypassThrottle 形参当前所有调用方均传 false，
                // 围栏报警实际同样走这个 2 秒窗口（宣称"不限流"与实现不符，
                // 若要围栏豁免需调用方显式传 true）。
                if (!bypassThrottle && steadyNow - last < kAlarmThrottleNs) {
                    continue;  // 在限流窗口内，跳过
                }
                lastAlarmTime_[key] = steadyNow;  // 记录本次报警的单调时钟值

                // 创建报警记录
                AlarmRecord alarm;
                alarm.id = QUuid::createUuid().toString().remove('{').remove('}').remove('-');
                alarm.channel = channel;
                alarm.clsId = det.cls_id;
                alarm.className = className;
                alarm.confidence = det.prop;
                alarm.alarmType = bypassThrottle ? "fence" : "detection";
                alarm.alarmLevel = bypassThrottle ? 2 : 3;

                // 确保timestamp有效(毫秒)
                long ts = results.time / 1000000LL;
                if (ts <= 0 || ts > 9999999999999LL) {
                    ts = QDateTime::currentMSecsSinceEpoch();
                }
                alarm.timestamp = ts;

                // 确保alarmTime有效
                QDateTime dt = QDateTime::fromMSecsSinceEpoch(ts);
                if (!dt.isValid() || dt.date().year() < 2020) {
                    dt = QDateTime::currentDateTime();
                }
                alarm.alarmTime = dt.toString(Qt::ISODate);

                alarm.status = "pending";

                // 调试输出
                qDebug() << "AlarmManager: Creating alarm - id:" << alarm.id
                         << "timestamp:" << alarm.timestamp
                         << "alarmTime:" << alarm.alarmTime
                         << "channel:" << alarm.channel
                         << "className:" << alarm.className;

                newAlarms.append(alarm);

                // 将触发报警的检测数据也存入 detections 表
                // 注意：只存报警相关的检测，而非所有检测结果，节省存储空间
                // 可通过 config.json 的 database.storeDetections 控制开关
                if (ConfigManager::instance().storeDetections() &&
                    DatabaseManager::instance().database().isOpen()) {
                    DetectionDAO detDao;
                    object_detect_result_list singleResult;
                    singleResult.time = results.time;
                    singleResult.id = results.id;
                    singleResult.count = 1;
                    singleResult.results[0] = det;
                    detDao.insertFromDetectResult(channel, singleResult, classNames_);
                }
            }
        }
    }
    return newAlarms;
}

// ============================================================================
// storeAndNotify: 存储报警记录并通知界面
// ============================================================================
// 这是报警系统的出口函数，由解码线程调用。
//
// 流程：
//   1. 加锁，将新报警添加到内存列表 alarms_
//   2. 限制报警数量（最多 1000 条，超出时从头部 removeFirst 淘汰最旧的，
//      FIFO 环形语义；removeFirst 为 O(n) 搬移，但容量小且报警低频，可接受）
//   3. 解锁（避免发信号时死锁）
//   4. 写入 SQLite 数据库（通过 AlarmDAO）
//   5. 发送 alarmGenerated 信号通知UI显示新报警
//   6. 发送 statsUpdated 信号通知UI刷新统计数据
//
// 线程安全：
//   - alarms_ 的读写在 mutex_ 保护下
//   - 信号/槽跨线程时自动排队（AlarmRecord 已 qRegisterMetaType），Qt 保证线程安全
//   - 先解锁再发信号，避免信号槽同步回调重入本管理器取锁而死锁
//
// 注意：
//   - 如果数据库未初始化（dbInitialized_=false），只存内存不写数据库
//   - 截图路径 imagePath 已由调用方（ffmpeg_video_decoder）填充
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

    // 写入数据库
    if (dbInitialized_ && !alarms.isEmpty()) {
        AlarmDAO dao;
        int64_t inserted = dao.insertAlarms(alarms);
        qDebug() << "AlarmManager: Inserted" << inserted << "alarms to database";
    } else if (!dbInitialized_) {
        qWarning() << "AlarmManager: Database not initialized, alarms not saved";
    }

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
        if (a.status == "pending") count++;
    }
    return count;
}

// 确认所有报警（将所有报警标记为已确认）
void AlarmManager::acknowledgeAll()
{
    QVector<QString> ids;
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.status != "rectified") {
                a.status = "rectified";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
                ids.append(a.id);
            }
        }
    }
    // 写入数据库
    if (dbInitialized_ && !ids.isEmpty()) {
        AlarmDAO dao;
        for (const QString &id : ids) {
            dao.updateStatus(id, "rectified");
        }
    }
    emit statsUpdated();
}

// 确认单条报警（标记为已确认）
bool AlarmManager::acknowledgeAlarm(int index)
{
    QString id;
    {
        QMutexLocker lock(&mutex_);
        if (index < 0 || index >= alarms_.size()) {
            return false;
        }
        alarms_[index].status = "rectified";
        alarms_[index].updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
        id = alarms_[index].id;
    }
    // 写入数据库
    if (dbInitialized_ && !id.isEmpty()) {
        AlarmDAO dao;
        dao.updateStatus(id, "rectified");
    }
    emit statsUpdated();
    return true;
}

// 清空所有报警
void AlarmManager::clearAlarms()
{
    {
        QMutexLocker lock(&mutex_);
        alarms_.clear();
    }
    // 清空数据库
    if (dbInitialized_) {
        AlarmDAO dao;
        dao.clearAll();
    }
    emit statsUpdated();
}

// 删除单条报警（用于误报标记）
bool AlarmManager::removeAlarm(int index)
{
    QString id;
    {
        QMutexLocker lock(&mutex_);
        if (index < 0 || index >= alarms_.size()) {
            return false;
        }
        id = alarms_[index].id;
        alarms_.removeAt(index);
    }
    // 从数据库删除
    if (dbInitialized_ && !id.isEmpty()) {
        AlarmDAO dao;
        dao.remove(id);
    }
    return true;
}

// 标记为误报
bool AlarmManager::markAsFalsePositive(int index)
{
    QString id;
    {
        QMutexLocker lock(&mutex_);
        if (index < 0 || index >= alarms_.size()) {
            return false;
        }
        alarms_[index].status = "false_alarm";
        alarms_[index].updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
        id = alarms_[index].id;
    }
    // 写入数据库
    if (dbInitialized_ && !id.isEmpty()) {
        AlarmDAO dao;
        dao.markFalseAlarm(id);
    }
    emit statsUpdated();
    return true;
}

// 确认单条报警（按ID，标记为已确认）- 推荐使用
bool AlarmManager::acknowledgeAlarmById(const QString &id)
{
    if (id.isEmpty()) return false;
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id == id) {
                a.status = "rectified";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
                found = true;
                break;
            }
        }
    }
    // 写入数据库
    if (dbInitialized_ && found) {
        AlarmDAO dao;
        dao.updateStatus(id, "rectified");
    }
    emit statsUpdated();
    return found;
}

// 删除单条报警（按ID）
bool AlarmManager::removeAlarmById(const QString &id)
{
    if (id.isEmpty()) return false;
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (int i = 0; i < alarms_.size(); ++i) {
            if (alarms_[i].id == id) {
                alarms_.removeAt(i);
                found = true;
                break;
            }
        }
    }
    // 从数据库删除
    if (dbInitialized_ && found) {
        AlarmDAO dao;
        dao.remove(id);
    }
    return found;
}

// 标记为误报（按ID）- 推荐使用
bool AlarmManager::markAsFalsePositiveById(const QString &id)
{
    if (id.isEmpty()) return false;
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id == id) {
                a.status = "false_alarm";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
                found = true;
                break;
            }
        }
    }
    // 写入数据库
    if (dbInitialized_ && found) {
        AlarmDAO dao;
        dao.markFalseAlarm(id);
    }
    emit statsUpdated();
    return found;
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

// ============================================================================
// 数据库支持
// ============================================================================

// ============================================================================
// initDatabase: 初始化数据库连接
// ============================================================================
bool AlarmManager::initDatabase(const QString &dbPath)
{
    QMutexLocker lock(&mutex_);
    if (dbInitialized_) {
        return true;
    }

    if (DatabaseManager::instance().initialize(dbPath)) {
        dbInitialized_ = true;
        qInfo() << "AlarmManager: Database initialized:" << dbPath;
        return true;
    }

    qWarning() << "AlarmManager: Failed to initialize database";
    return false;
}

// ============================================================================
// loadAlarmsFromDatabase: 从数据库加载历史报警（程序启动时调用）
// ============================================================================
// 流程：
//   1. 通过 AlarmDAO 查询所有报警（按时间倒序）
//   2. 加锁，将数据库报警合并到内存列表（避免重复）
//   3. 去重逻辑：比较 id（UUID主键）
//   4. 限制总数量不超过 1000 条
//
// 使用场景：
//   - 程序启动时调用，恢复上次退出前的报警记录
//   - 重启后用户仍可查看所有状态的报警（包括已确认、已处置的）
//
// ============================================================================
void AlarmManager::loadAlarmsFromDatabase(int limit)
{
    if (!dbInitialized_) {
        return;
    }

    AlarmDAO dao;
    QVector<AlarmRecord> dbAlarms = dao.queryAll(limit);

    QMutexLocker lock(&mutex_);
    // 合并数据库报警到内存（避免重复）
    for (const AlarmRecord &alarm : dbAlarms) {
        bool exists = false;
        for (const AlarmRecord &existing : alarms_) {
            if (existing.id == alarm.id) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            alarms_.append(alarm);
        }
    }

    // 限制报警数量
    while (alarms_.size() > 1000) {
        alarms_.removeFirst();
    }

    qInfo() << "AlarmManager: Loaded" << dbAlarms.size() << "alarms from database";
}

// ============================================================================
// syncToDatabase: 同步内存报警到数据库（程序退出时调用）
// ============================================================================
// 将内存中所有报警记录批量写入数据库，确保不丢失。
//
// 使用场景：
//   - 程序正常退出时调用（main.cpp 的 cleanup 逻辑）
//   - 保证内存中的报警数据持久化
//
// 注意：必须先复制数据再解锁，避免与 DAO 的锁产生死锁
//
// ============================================================================
void AlarmManager::syncToDatabase()
{
    if (!dbInitialized_) {
        return;
    }

    QVector<AlarmRecord> alarmsCopy;
    {
        QMutexLocker lock(&mutex_);
        if (alarms_.isEmpty()) {
            return;
        }
        alarmsCopy = alarms_;  // 复制数据
    } // 解锁后再调用 DAO

    AlarmDAO dao;
    int64_t inserted = dao.insertAlarms(alarmsCopy);
    qInfo() << "AlarmManager: Synced" << inserted << "alarms to database";
}

// ============================================================================
// cleanOldData: 清理N天前的数据库数据
// ============================================================================
void AlarmManager::cleanOldData(int daysToKeep)
{
    if (!dbInitialized_) {
        return;
    }

    DatabaseManager::instance().cleanOldDetections(daysToKeep);
    DatabaseManager::instance().cleanOldAlarms(daysToKeep);
    qInfo() << "AlarmManager: Cleaned data older than" << daysToKeep << "days";
}
