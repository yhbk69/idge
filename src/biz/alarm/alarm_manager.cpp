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
//   ┌──────────────────────────────────────────────────────────────┐
//   │  ffmpeg_video_decoder.cpp::decodeLoop()                     │
//   │  │                                                          │
//   │  ├── 每3帧做一次YOLO检测（每个槽位一路模型）                │
//   │  │                                                          │
//   │  ├── 逐槽位 ingest(channel, od, slotCtx)   → 普通检测报警   │
//   │  │   ├── 按该槽位模型的报警类别判定 + 2 秒限流窗口          │
//   │  │   └── 顺手收集本帧围栏候选（类别 = geofence.alarmClass） │
//   │  │                                                          │
//   │  ├── 槽位循环结束后，一次通道级合并围栏判定                 │
//   │  │   └── 几何命中 → ingestFenceAlarm()（独立 10 秒窗口）    │
//   │  │                                                          │
//   │  └── AlarmManager::storeAndNotify(newAlarms)                │
//   │  │   ├── 添加到内存 alarms_ 列表（最多1000条）              │
//   │  │   ├── AlarmDAO::insertAlarms() → 写入SQLite数据库        │
//   │  │   └── emit alarmGenerated() → 通知UI显示报警             │
//   └──────────────────────────────────────────────────────────────┘
//
// 类别判定的两个入口（历史易混淆）：
//   - 普通报警：只看该槽位模型的 alarm_class（SlotContext.alarmClass，值来自
//     config.json 的 video.models[<模型库id>].alarm_class）。
//   - 围栏报警：FenceManager 的 geofence.alarmClass 单值，判定走几何命中。
//   两者各用自己的限流窗口（2 秒检测限流 / geofence.throttleSeconds 围栏限流），互不影响。
//
// ============================================================================

#include "alarm_manager.h"
#include "perf_probe.h"
#include "ConfigManager.h"
#include "construction_session.h"
#include "database_manager.h"
#include "alarm_dao.h"
#include "detection_dao.h"
#include <QDateTime>
#include <QDate>
#include <QDebug>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {

// ============================================================================
// AlarmDbWriter - 报警落库后台线程
// ============================================================================
// 为什么需要：storeAndNotify() 原先在解码线程里同步执行 AlarmDAO::insertAlarms，
// 实测一批 41ms（SQLite 每次提交都要 fsync，WAL 也省不掉），这 41ms 里该通道不产帧。
// 移走它并没有消除"每秒卡一次"——板端实测剩下的间隔主要来自 4 路 17~23fps 的出帧
// 抖动本身；但解码线程被一次数据库提交堵住这件事，无论如何不该出现在取帧链路上。
//
// 语义保持不变：
//   - 只改"什么时候写"，不改"写什么"。insertAlarms 是 INSERT OR REPLACE 按 id 幂等，
//     程序退出时 AlarmManager::syncToDatabase() 还会把内存里的全量再补一遍，
//     所以队列即使没排干也不会丢报警。
//   - 单线程顺序消费，同一批次内顺序与原来一致；ClipWriter 在报警 +2s 后才做
//     attachVideoClip 的 UPDATE，队列延迟是毫秒级，不会反超。
//   - DatabaseManager 按线程分配连接，因此本线程内 new AlarmDAO 是安全的
//     （见 attachVideoClip 上方同样的说明）。
// ============================================================================
class AlarmDbWriter {
public:
    static AlarmDbWriter &get()
    {
        static AlarmDbWriter w;   // 首次提交时启动，进程内唯一
        return w;
    }

    // 入队后返回当前积压批数，调用方只用它做诊断
    int submit(const QVector<AlarmRecord> &batch)
    {
        int depth;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            q_.emplace_back(batch);
            depth = static_cast<int>(q_.size());
        }
        cv_.notify_one();
        return depth;
    }

private:
    AlarmDbWriter() { th_ = std::thread([this] { run(); }); }
    ~AlarmDbWriter()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();   // 退出前把已入队的都写完
    }

    void run()
    {
        for (;;) {
            QVector<AlarmRecord> batch;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) {
                    if (stop_) return;
                    continue;
                }
                batch = std::move(q_.front());
                q_.pop_front();
            }
            AlarmDAO dao;   // 与 attachVideoClip 一致：后台线程里按次建 DAO
            qDebug() << "AlarmManager: Inserted" << dao.insertAlarms(batch)
                     << "alarms to database";
        }
    }

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<QVector<AlarmRecord>> q_;
    bool stop_ = false;
};

} // namespace

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
// 只负责注册 Qt 元类型（见 instance()）；报警判定需要的类别信息全部由
// 解码器随检测结果传入（见 ingest 的槽位类名表/报警类别），本类不再自己读配置。
// ============================================================================
AlarmManager::AlarmManager(QObject *parent)
    : QObject(parent)
{
}

// ============================================================================
// 报警记录构造与检测明细（普通报警与围栏报警共用）
// ============================================================================

namespace {

// 单调时钟纳秒，与 kAlarmThrottleNs / 围栏节流窗口同量纲
long steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 把 cls_id 翻成该槽位模型的类别名。V2 里每个模型的标签空间不同，
// 没有全局兜底表：越界就是 "cls_N"。
QString slotClassName(const AlarmManager::SlotContext &ctx, int clsId)
{
    if (clsId >= 0 && clsId < ctx.classNames.size())
        return ctx.classNames[clsId];
    return QString("cls_%1").arg(clsId);
}

// 组装一条报警记录。timestamp/alarmTime 的合法性兜底逻辑在此收敛：
// results.time 是推理管线透传的 epoch 纳秒，异常（0/越界）时退回当前墙钟。
AlarmRecord makeAlarmRecord(int channel, const object_detect_result &det,
                            const QString &className, long timeNs,
                            const QString &alarmType, int alarmLevel)
{
    AlarmRecord alarm;
    alarm.id = QUuid::createUuid().toString().remove('{').remove('}').remove('-');
    alarm.channel = channel;
    alarm.clsId = det.cls_id;
    alarm.className = className;
    alarm.confidence = det.prop;
    alarm.alarmType = alarmType;
    alarm.alarmLevel = alarmLevel;

    long ts = timeNs / 1000000LL;
    if (ts <= 0 || ts > 9999999999999LL)
        ts = QDateTime::currentMSecsSinceEpoch();
    alarm.timestamp = ts;

    QDateTime dt = QDateTime::fromMSecsSinceEpoch(ts);
    if (!dt.isValid() || dt.date().year() < 2020)
        dt = QDateTime::currentDateTime();
    alarm.alarmTime = dt.toString(Qt::ISODate);

    // create_time / update_time 在建行这一刻就写死，而不是留给 DAO 兜底或留着空串：
    // Pad 接口 v1 把 update_time 当**断线补漏的游标**（§3.8"按 updatedAt 过滤、增量必须
    // 包含新增报警"）。空 update_time 会让新报警被 `update_time >= 游标` 整批漏掉，
    // 对端重连后看不到刚才那几条。状态变更仍走各自的 UPDATE 把它往后推。
    alarm.createTime = alarm.alarmTime;
    alarm.updateTime = alarm.alarmTime;

    alarm.status = "pending";

    // 施工信息在**产生这一刻**固化（不等到显示/上行时再回查会话）：
    // 退出登录后重传的那条报警，仍带着它产生时的施工信息。
    // 未登录时 snapshot 是空对象，两列都留空 ⇒ 门挡住的正是这里。
    const session::ConstructionInfo construction =
        session::ConstructionSession::instance().snapshot();
    alarm.workerName = construction.personName;
    // 施工计划身份（v1 的 taskId）＝二维码里的 plan.taskId（B2 定案：对外只叫 taskId）
    alarm.taskId = construction.taskId;
    if (!construction.raw.isEmpty() || !construction.taskId.isEmpty()) {
        alarm.constructionJson = QString::fromUtf8(
            QJsonDocument(construction.toJson()).toJson(QJsonDocument::Compact));
    }
    return alarm;
}

// 触发报警的检测数据写入 detections 表（只存报警相关的那一条，节省空间；
// 开关为 config.json 的 database.storeDetections）。
// slot 透传到 singleResult.id，落库后是 detections.frame_id 列。
void storeDetectionDetail(int channel, const object_detect_result &det,
                          const QStringList &classNames, long timeNs, int slot)
{
    if (!ConfigManager::instance().storeDetections() ||
        !DatabaseManager::instance().database().isOpen()) {
        return;
    }
    DetectionDAO detDao;
    object_detect_result_list singleResult;
    singleResult.time = timeNs;
    singleResult.id = slot;
    singleResult.count = 1;
    singleResult.results[0] = det;
    detDao.insertFromDetectResult(channel, singleResult, classNames);
}

} // namespace

// ============================================================================
// 这是报警系统的入口函数，由解码线程调用。
//
// ============================================================================
// ingest: 接收检测结果，判断是否需要生成报警
// ============================================================================
// 这是普通检测报警的入口函数，由解码线程按槽位调用。
//
// 参数：
//   - channel: 视频通道编号（0, 1, 2, ...）
//   - results: 检测结果列表（包含多个目标的类别、置信度、位置）
//   - ctx:     该槽位的类别名表 + 报警类别（由解码器构造，见 SlotContext）
//
// 返回：新生成的报警列表（未存储，由调用方决定何时存储）
//
// 处理流程：
//   1. 加锁保护共享数据（mutex_）
//   2. 遍历所有检测结果：
//      a. 将 cls_id 用 ctx.classNames 转换为类别名称（如 "person"、"helmet"）
//      b. 统计各类别检测次数（用于仪表盘展示）
//      c. 检查是否需要生成报警：
//         - 类别必须等于该槽位模型的报警类别（ctx.alarmClass，空=只画框不报警）
//         - 同通道同槽位同类别必须超过限流间隔（2秒）
//      d. 创建 AlarmRecord 结构体
//   3. 返回新报警列表
//
// 线程安全：
//   - 整个函数在 mutex_ 保护下执行
//   - 只在解码线程调用，不存在竞争
//   - ctx 是调用方栈上的对象，本类不持有、不缓存，因此没有跨通道串配置的问题
//
// 围栏报警不在这里判定（围栏是通道级、跨槽位的几何判定，见 ingestFenceAlarm）。
//
// ============================================================================
QVector<AlarmRecord> AlarmManager::ingest(int channel, const object_detect_result_list &results,
                                         const SlotContext &ctx)
{
    QVector<AlarmRecord> newAlarms;
    // 限流窗口计时用单调时钟：results.time 源自 system_clock 墙钟纳秒，
    // NTP 校时/手动改时间发生**回拨**时 `results.time - last` 变负，
    // 恒小于窗口阈值 → 同键报警可被抑制数天。steady_clock 只增不减，
    // 与墙钟彻底解耦（lastAlarmTime_ 因此改存 steady 纳秒，仅内存态）。
    const long steadyNow = steadyNowNs();
    {
        QMutexLocker lock(&mutex_);
        totalDetections_ += results.count;

        // 级联槽位（= TaskConfig.result_id），仅用于区分限流键：
        // 不同模型恰好同类号时不应互相压制报警
        const int slot = results.id;

        for (int i = 0; i < results.count; i++) {
            const object_detect_result &det = results.results[i];
            const QString className = slotClassName(ctx, det.cls_id);

            // 统计各类别检测次数
            classCount_[className]++;

            // 报警类别是"模型的参数"：空 = 这个模型只画框不报警
            if (ctx.alarmClass.isEmpty() || ctx.alarmClass != className)
                continue;

            // 去重限流：同通道同槽位同类别在限流窗口内只报一次
            // key 格式: "通道:槽位:类别ID"，如 "0:1:0" 表示通道 0、级联第 2 槽的 cls_0。
            // 槽位入键后，不同模型恰好同类号（helmet cls_1 与 vest cls_1）
            // 不再互相压制报警。
            QString key = QString("%1:%2:%3").arg(channel).arg(slot).arg(det.cls_id);
            long last = lastAlarmTime_.value(key, 0);  // 首次为 0，必然放行
            // 与 kAlarmThrottleNs 同为 steady 纳秒，量纲一致。
            if (steadyNow - last < kAlarmThrottleNs) {
                continue;  // 在限流窗口内，跳过
            }
            lastAlarmTime_[key] = steadyNow;  // 记录本次报警的单调时钟值

            AlarmRecord alarm = makeAlarmRecord(channel, det, className, results.time,
                                                QStringLiteral("detection"),
                                                ConfigManager::instance().alarmLevel(QStringLiteral("detection")));

            // 调试输出
            qDebug() << "AlarmManager: Creating alarm - id:" << alarm.id
                     << "timestamp:" << alarm.timestamp
                     << "alarmTime:" << alarm.alarmTime
                     << "channel:" << alarm.channel
                     << "className:" << alarm.className;

            newAlarms.append(alarm);
            storeDetectionDetail(channel, det, ctx.classNames, results.time, slot);
        }
    }
    return newAlarms;
}

// ============================================================================
// ingestFenceAlarm: 电子围栏报警（闯入禁区 / 离岗）
// ============================================================================
// 由解码线程的"通道级合并围栏判定"对每个**新进入违规状态**的目标调用。与 ingest 不同：
//   - 类别不做匹配（能调到这里，说明调用方已按 geofence.alarmClass 选好类别、
//     判定该目标违规，并且已用 IoU 事件表判定这不是"同一个人还在违规状态里"），
//     本函数只负责限流与建记录；
//   - 限流走独立窗口 geofence.throttleSeconds（默认 10 秒），键
//     "通道:fence:角色:类别名"。用类名而非 "槽位:cls_id"，配置重排槽位后仍稳定；
//     **角色必须在键里**，否则同一个人先闯入禁区再被判离岗时，第二条会被第一条的
//     窗口吞掉——两种报警共用一个窗口就等于只有一种报得出来。
//   - alarmType 按角色分："fence_in"（闯入禁区）/ "fence_out"（离岗）。下游
//     AlarmDAO 按 alarmType 前缀 "fence" 恢复 isFenceAlarm，报警列表按它出中文文案，
//     上行报文原样把 alarm_type 发给对端。
//     "一次事件报一条"由调用方的进入沿判定保证，这个窗口是它之下的兜底：
//     框抖碎成多个事件、或两个人几乎同时违规时，最多 10 秒出一条。
//   历史：再之前是按每帧一条入库（bypassThrottle=true），40 秒出三百多条，已不可用。
//
// 统计（totalDetections_/classCount_）不在这里累加：同一批检测结果已经由
// 该槽位的 ingest 计过一次，围栏路径只补报警记录。
// ============================================================================
QVector<AlarmRecord> AlarmManager::ingestFenceAlarm(int channel, const object_detect_result &det,
                                                   const SlotContext &ctx, long timeNs,
                                                   geofence::FenceRole role)
{
    QVector<AlarmRecord> newAlarms;
    const QString className = slotClassName(ctx, det.cls_id);
    const QString roleTag = QString::fromLatin1(
            role == geofence::FenceRole::Workzone ? "out" : "in");
    const long windowNs = (long)ConfigManager::instance().geofenceThrottleSeconds()
                              * 1000000LL * 1000LL;
    const long steadyNow = steadyNowNs();
    {
        QMutexLocker lock(&mutex_);
        const QString key = QString("%1:fence:%2:%3").arg(channel).arg(roleTag).arg(className);
        if (steadyNow - lastAlarmTime_.value(key, 0) < windowNs)
            return newAlarms;   // 节流窗口内：进入沿抖动或多人几乎同时违规，只留第一条
        lastAlarmTime_[key] = steadyNow;

        const QString fenceType = QStringLiteral("fence_") + roleTag;
        AlarmRecord alarm = makeAlarmRecord(channel, det, className, timeNs,
                                            fenceType,
                                            ConfigManager::instance().alarmLevel(fenceType));
        alarm.isFenceAlarm = true;

        qDebug() << "AlarmManager: Creating fence alarm - id:" << alarm.id
                 << "channel:" << alarm.channel
                 << "type:" << alarm.alarmType
                 << "className:" << alarm.className
                 << "windowSeconds:" << ConfigManager::instance().geofenceThrottleSeconds();

        newAlarms.append(alarm);
        storeDetectionDetail(channel, det, ctx.classNames, timeNs, /*slot=*/-1);
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
            alarms_.push_back(a);
        }
        // 限制报警数量，最多保留 1000 条
        // 避免内存无限增长
        while (alarms_.size() > 1000) {
            alarms_.pop_front();  // 删除最早的报警 (O(1) for deque)
        }
    } // 解锁后再发信号，避免死锁

    // 写入数据库：交给后台线程，解码线程只入队（见 AlarmDbWriter 注释）
    if (dbInitialized_ && !alarms.isEmpty()) {
        const int depth = AlarmDbWriter::get().submit(alarms);
        if (perfProbe() && depth > 4) {
            qWarning() << "[PROBE] AlarmDbWriter backlog" << depth << "batches";
        }
    } else if (!dbInitialized_) {
        qWarning() << "AlarmManager: Database not initialized, alarms not saved";
    }

    // 通知界面更新
    for (const AlarmRecord &alarm : alarms)
        emit alarmGenerated(alarm);  // 发送报警信号
    emit statsUpdated();  // 发送统计更新信号
}

// ============================================================================
// attachVideoClip: 报警视频片段落盘后回填 videoPath
// ============================================================================
// 由 ClipWriter 后台线程调用（报警后 2 秒），此时 storeAndNotify 早已完成，
// 所以这里的 UPDATE 必然能命中已存在的行。
//
// 内存按"扫到的改"、DB 按"传入的改"：内存列表只留最近 1000 条，
// 若某条已被挤出内存，它在 DB 里的记录仍然应该拿到视频路径。
// DatabaseManager 为每个线程分配独立连接，因此后台线程里 new AlarmDAO 是安全的。
//
// webPath（.mp4 副本）只往下游信号里带，不进 DB 也不进内存记录：那两个字段是界面与
// 报警详情用来定位本地文件的，换成副本路径会让点开报警找不到图旁边那段视频。
// ============================================================================
void AlarmManager::attachVideoClip(const QStringList &alarmIds, const QString &videoPath,
                                   const QString &webPath)
{
    if (alarmIds.isEmpty() || videoPath.isEmpty()) return;

    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (alarmIds.contains(a.id)) {
                a.videoPath = videoPath;
            }
        }
    }

    if (dbInitialized_) {
        AlarmDAO dao;
        for (const QString &id : alarmIds) {
            dao.updateVideoPath(id, videoPath);
        }
    }

    // 只是"通知"，不改上面的 UPDATE 行为：报警上行的第二段（片段）靠这一刻才拿到路径
    emit videoClipAttached(alarmIds, videoPath, webPath);
}

void AlarmManager::reportVideoClipSkipped(const QStringList &alarmIds, const QString &reason)
{
    if (alarmIds.isEmpty()) {
        return;
    }
    emit videoClipSkipped(alarmIds, reason);
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

// 这一路在不在线（v1 §6/§7 的 status 判据）
bool AlarmManager::isChannelOnline(int channel) const
{
    QMutexLocker lock(&mutex_);
    return onlineChannels_.contains(channel);
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
    QElapsedTimer probe; probe.start();
    QMutexLocker lock(&mutex_);
    const qint64 waitMs = probe.elapsed();
    QVector<AlarmRecord> result;
    result.reserve(alarms_.size());
    for (const auto &a : alarms_) {
        result.append(a);
    }
    // 卡顿探针：锁等待 = 解码线程持锁写库时 GUI 线程被挡住的时长
    if (perfProbe() && (waitMs > 10 || probe.elapsed() > 20)) {
        qWarning() << "[PROBE] AlarmManager::alarms() lockWait" << waitMs
                   << "ms total" << probe.elapsed() << "ms size" << result.size();
    }
    return result;
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
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.status != "rectified" && a.status != "disposed") {
                a.status = "rectified";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
            }
        }
    }
    // 一条 UPDATE 收掉库里全部未确认。原先这里是"遍历内存 1000 条 → 逐条 UPDATE"：
    // 实测库里 27858 条 pending，按钮点下去只改了 1000 条，其余照旧弹未确认角标。
    if (dbInitialized_) {
        AlarmDAO dao;
        dao.acknowledgeAllPending();
    }
    emit statsUpdated();
    // 这里**故意不发** alarmStatusChanged：这一条是"一句 UPDATE 收掉库里全部未确认"，
    // 改成逐条发就等于给 Pad 一次推几千帧 alarm_update（而且要先查出那几千个 id）。
    // 这些行的状态由下一次 sync_result 拉平——P1-2 修好之后那条路才是真的通的。
    // 单条确认/误报走 alarm_update，见 acknowledgeAlarmById 与 markAsFalsePositiveById。
}

// 确认单条报警（标记为已确认）
bool AlarmManager::acknowledgeAlarm(int index)
{
    QString id;
    {
        QMutexLocker lock(&mutex_);
        if (index < 0 || index >= static_cast<int>(alarms_.size())) {
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
        if (index < 0 || index >= static_cast<int>(alarms_.size())) {
            return false;
        }
        id = alarms_[index].id;
        alarms_.erase(alarms_.begin() + index);
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
        if (index < 0 || index >= static_cast<int>(alarms_.size())) {
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
    bool foundInRam = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id == id) {
                a.status = "rectified";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
                foundInRam = true;
                break;
            }
        }
    }
    // 库是权威，所以写库**不以"内存里找得到"为前提**：alarms_ 只留最近 1000 条，
    // 而报警列表现在能翻到几万条之外的历史行。沿用旧的 `if (found)` 闸门，
    // 结果就是点老报警的「已确认」什么都没发生、界面重载后状态照旧 —— 静默失效。
    bool dbOk = false;
    if (dbInitialized_) {
        AlarmDAO dao;
        dbOk = dao.updateStatus(id, "rectified");
    }
    emit statsUpdated();
    const bool changed = foundInRam || dbOk;
    // 报告 P1-1：状态写成功了就发一条带记录的信号，Pad 靠它把那条从 PENDING 换成
    // RECTIFIED。以前只有 statsUpdated，对端拿不到"是哪一条"，只能等下次重连补漏。
    if (changed) {
        const AlarmRecord snapshot = recordForId(id);
        if (!snapshot.id.isEmpty()) {
            emit alarmStatusChanged(snapshot);
        }
    }
    return changed;
}

// 删除单条报警（按ID）
bool AlarmManager::removeAlarmById(const QString &id)
{
    if (id.isEmpty()) return false;
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
            if (it->id == id) {
                alarms_.erase(it);
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
bool AlarmManager::markAsFalsePositiveById(const QString &id, const QString &personId,
                                           const QString &personName,
                                           const QString &markedAtInternal)
{
    if (id.isEmpty()) return false;
    bool foundInRam = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id == id) {
                foundInRam = true;
                a.status = "false_alarm";
                a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
                // 只在真给了处置人时才覆盖：本机那条路径三个参数全空，
                // 无脑写空串会把 Pad 之前落进去的工号抹掉（内存副本会随关机回写进库）。
                if (!personId.isEmpty()) a.disposePersonId = personId;
                if (!personName.isEmpty()) a.disposeUserName = personName;
                if (!markedAtInternal.isEmpty()) a.disposeTime = markedAtInternal;
                break;
            }
        }
    }
    // 与 acknowledgeAlarmById 同理：内存窗口外的历史行也要能标误报。
    bool dbOk = false;
    if (dbInitialized_) {
        AlarmDAO dao;
        dbOk = dao.markFalseAlarm(id, QString(), personId, personName, markedAtInternal);
    }
    emit statsUpdated();
    // changed 的判据沿用改造前那条（内存命中**或**库改成功都算）：
    // 刚产生的报警可能还在 AlarmDbWriter 的队列里，库里查不到但内存里有，
    // 那种情况下判 false 会让本机「误报」按钮按下去没反应。
    // ⚠ 「库里这条已经是终态所以 UPDATE 了 0 行」在这里仍可能被 foundInRam 顶成 true ——
    //   那是刻意的（UI 那条路径要照旧能点），而 §4.3 的 409/2003 由 HTTP 路由
    //   先 queryById 判出来，不依赖这里的返回值。
    const bool changed = foundInRam || dbOk;
    if (changed) {
        const AlarmRecord snapshot = recordForId(id);
        if (!snapshot.id.isEmpty()) {
            emit alarmStatusChanged(snapshot);
        }
    }
    return changed;
}

// v1 §4.2 的处置入口：先改库、改成了才动内存副本，最后发信号
bool AlarmManager::disposeAlarmById(const QString &id, const QString &measure,
                                    const QString &remark, const QString &personId,
                                    const QString &personName, const QString &photoPathsJson,
                                    const QString &disposedAtInternal, QString *err)
{
    if (id.isEmpty()) {
        if (err) *err = QString::fromUtf8("alarmId 是空的");
        return false;
    }
    if (!dbInitialized_) {
        if (err) *err = QString::fromUtf8("报警库没开，处置无法落库");
        return false;
    }
    bool dbOk = false;
    {
        AlarmDAO dao;
        // WHERE 里带了"非终态"那条守卫（见 AlarmDAO::dispose），所以库里已是 disposed /
        // false_alarm 的行返回 0 行 → 这里就是 false，处置不会被写进去。
        dbOk = dao.dispose(id, measure, personId, personName, remark, photoPathsJson,
                           disposedAtInternal);
    }
    if (!dbOk) {
        // 库里没改成，就**不动内存、也不发信号**。
        // 顺序为什么是"库优先"而不是像 acknowledgeAlarmById 那样两头各写各的：
        // 处置是一次有表单的写（照片已经落盘、Pad 已经把界面切成已处置了），
        // 让内存副本先动而库没跟上，关机时 syncToDatabase() 会把这份"没落库的处置"
        // 连带照片路径一起 INSERT OR REPLACE 进去——那等于伪造一次成功的处置。
        // 反过来（现在的写法）最坏只是这次没写成，对端拿到 500 重试即可。
        if (err) {
            *err = QString::fromUtf8("库里没有这条报警，或它已经是终态（disposed/false_alarm 不再改）");
        }
        return false;
    }
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id != id)
                continue;
            a.status = QStringLiteral("disposed");
            a.disposeResult = measure;
            a.disposePersonId = personId;
            a.disposeUserName = personName;
            a.disposePhoto = photoPathsJson;
            a.disposeTime = disposedAtInternal.isEmpty()
                ? QDateTime::currentDateTime().toString(Qt::ISODate) : disposedAtInternal;
            if (!remark.isEmpty())
                a.remark = remark;
            a.updateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
            break;
        }
    }
    emit statsUpdated();
    const AlarmRecord snapshot = recordForId(id);
    if (!snapshot.id.isEmpty())
        emit alarmStatusChanged(snapshot);   // §4.2 成功后广播 alarm_update
    return true;
}

// v1 §4.4 的已读入口：先改库、改成了才动内存副本，最后发信号（与 disposeAlarmById 同一顺序）
bool AlarmManager::markAlarmReadById(const QString &id, const QString &readAtInternal)
{
    if (id.isEmpty() || !dbInitialized_)
        return false;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    const QString candidate = readAtInternal.isEmpty() ? now : readAtInternal;
    bool dbOk = false;
    {
        AlarmDAO dao;
        // 库里那条 CASE WHEN 保证"保留最早"，所以重复上报同一句 readAt 是安全的空转
        dbOk = dao.markRead(id, candidate);
    }
    if (!dbOk)
        return false;   // 库里没这条：不报成功，也不发信号
    {
        QMutexLocker lock(&mutex_);
        for (auto &a : alarms_) {
            if (a.id != id)
                continue;
            // 内存侧同样"保留最早"：库里那条 CASE WHEN 是同一个判据，
            // 两处不一致就会出现"库里的已读比界面上的晚"这种对不上号的现象
            if (a.readTime.isEmpty() || a.readTime > candidate)
                a.readTime = candidate;
            a.updateTime = now;
            break;
        }
    }
    emit alarmStatusChanged(recordForId(id));   // §4.4 要边缘设备把已读同步给其他端
    return true;
}

// 取某条报警的当前记录（给 alarmStatusChanged 当载荷）
// 内存窗口里那份是刚改过的、最权威，优先用它；窗口外的历史行只能回库读一次。
AlarmRecord AlarmManager::recordForId(const QString &id)
{
    {
        QMutexLocker lock(&mutex_);
        for (const auto &a : alarms_) {
            if (a.id == id) {
                return a;
            }
        }
    }
    if (dbInitialized_) {
        AlarmDAO dao;
        return dao.queryById(id);
    }
    return AlarmRecord();
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
    // 合并数据库报警到内存（用 hash 去重，O(n+m) 而非 O(n*m)）
    QSet<QString> existingIds;
    existingIds.reserve(alarms_.size());
    for (const AlarmRecord &a : alarms_) {
        existingIds.insert(a.id);
    }
    for (const AlarmRecord &alarm : dbAlarms) {
        if (!existingIds.contains(alarm.id)) {
            alarms_.push_back(alarm);
            existingIds.insert(alarm.id);
        }
    }

    // 限制报警数量
    while (alarms_.size() > 1000) {
        alarms_.pop_front();
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
        if (alarms_.empty()) {
            return;
        }
        alarmsCopy.reserve(alarms_.size());
        for (const auto &a : alarms_) {
            alarmsCopy.append(a);
        }
    } // 解锁后再调用 DAO

    AlarmDAO dao;
    int64_t inserted = dao.insertAlarms(alarmsCopy);
    qInfo() << "AlarmManager: Synced" << inserted << "alarms to database";
}

// ============================================================================
// pruneAlarmMedia: 按天目录裁剪报警媒体（抓拍图 + 视频片段）
// ============================================================================
// 两道闸门，缺一不可：
//   1) 天数：与 DB 的 cleanOldAlarms 同口径，过期归档整目录回收；
//   2) 容量：4 路常驻硬编在报警密集的现场一天就能写出几十 GB（板端实测
//      单段 1.2MB / 40 秒 12 段），分区写满会直接拖垮检测与落库主流程，
//      所以超过上限时从最旧的一天开始删，直到落回上限内。
// 只动"目录名能按 yyyyMMdd 解析"的目录，现场手工放的其它子目录一律不碰。
// ============================================================================
static const qint64 kAlarmMediaMaxBytes = 4LL * 1024 * 1024 * 1024;   // 4 GB

static qint64 dirBytes(const QString &path)
{
    qint64 total = 0;
    QDir dir(path);
    const QFileInfoList files = dir.entryInfoList(QDir::Files);
    for (const QFileInfo &fi : files) {
        total += fi.size();
    }
    return total;
}

static void pruneAlarmMedia(int daysToKeep)
{
    QDir root(RuntimePaths::alarmsDir());
    if (!root.exists()) {
        return;
    }

    QStringList dayDirs;                      // 目录名（yyyyMMdd），从旧到新
    const QFileInfoList entries = root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        if (QDate::fromString(fi.fileName(), "yyyyMMdd").isValid()) {
            dayDirs << fi.fileName();
        }
    }
    dayDirs.sort();                           // yyyyMMdd 字典序即时间序

    // 闸门 1：过期归档
    const QDate cutoff = QDate::currentDate().addDays(-qMax(1, daysToKeep));
    while (!dayDirs.isEmpty() && QDate::fromString(dayDirs.first(), "yyyyMMdd") < cutoff) {
        const QString path = root.absoluteFilePath(dayDirs.takeFirst());
        if (!QDir(path).removeRecursively()) {
            qWarning() << "AlarmManager: prune alarm media failed" << path;
        }
    }

    // 闸门 2：容量上限（天数留着也可能超，例如当天就爆）
    QList<qint64> sizes;
    sizes.reserve(dayDirs.size());
    qint64 total = 0;
    for (const QString &name : dayDirs) {
        const qint64 sz = dirBytes(root.absoluteFilePath(name));
        sizes << sz;
        total += sz;
    }
    for (int i = 0; i < dayDirs.size() && total > kAlarmMediaMaxBytes; ++i) {
        const QString path = root.absoluteFilePath(dayDirs[i]);
        if (QDir(path).removeRecursively()) {
            total -= sizes[i];
            qInfo() << "AlarmManager: alarm media over cap, dropped" << dayDirs[i];
        } else {
            qWarning() << "AlarmManager: prune alarm media failed" << path;
            break;   // 删不动就别原地打转
        }
    }
}

// ============================================================================
// cleanOldData: 清理N天前的数据库数据 + 报警媒体文件
// ============================================================================
// 报警媒体目录 = data/alarms/yyyyMMdd/（抓拍图 + 视频片段，命名与归档同规则），
// 与 DB 里删掉的过期报警行一一对应，所以按同一个保留天数一起裁剪：
// 只留 DB，磁盘会被每天几十到上千个 1MB 级片段慢慢填满。
// ============================================================================
void AlarmManager::cleanOldData(int daysToKeep)
{
    pruneAlarmMedia(daysToKeep);

    if (!dbInitialized_) {
        return;
    }

    DatabaseManager::instance().cleanOldDetections(daysToKeep);
    DatabaseManager::instance().cleanOldAlarms(daysToKeep);
    qInfo() << "AlarmManager: Cleaned data older than" << daysToKeep << "days";
}

// ============================================================================
// resetThrottles: 退出登录时清空限流窗口（只动内存，不动 DB 与报警列表）
// ============================================================================
void AlarmManager::resetThrottles()
{
    QMutexLocker lock(&mutex_);
    const int keys = lastAlarmTime_.size();
    lastAlarmTime_.clear();
    qInfo() << "AlarmManager: throttle windows cleared (" << keys << "keys)";
}
