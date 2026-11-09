// ============================================================================
// test_database.cpp - 数据库模块测试程序
// ============================================================================
//
// 用法：
//   ./test_database
//
// 功能：
//   1. 测试数据库初始化
//   2. 测试检测信息插入和查询
//   3. 测试报警信息插入和查询
//   4. 测试统计功能
//   5. 测试数据清理
//
// ============================================================================

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSet>
#include <iostream>

#include "database_manager.h"
#include "detection_dao.h"
#include "alarm_dao.h"

// 测试计数
static int testsPassed = 0;
static int testsFailed = 0;

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            testsPassed++; \
            std::cout << "[PASS] " << message << std::endl; \
        } else { \
            testsFailed++; \
            std::cout << "[FAIL] " << message << std::endl; \
        } \
    } while(0)

// ============================================================================
// 测试数据库初始化
// ============================================================================
void testDatabaseInit()
{
    std::cout << "\n=== 测试数据库初始化 ===" << std::endl;

    // 使用内存数据库进行测试
    bool ok = DatabaseManager::instance().initialize(":memory:");
    TEST_ASSERT(ok, "数据库初始化");
    TEST_ASSERT(DatabaseManager::instance().isOpen(), "数据库已打开");
}

// ============================================================================
// 测试检测信息操作
// ============================================================================
void testDetectionOperations()
{
    std::cout << "\n=== 测试检测信息操作 ===" << std::endl;

    DetectionDAO dao;
    long now = QDateTime::currentMSecsSinceEpoch();

    // 测试插入
    DetectionRecord record;
    record.timestamp = now;
    record.channel = 0;
    record.frameId = 1001;
    record.classId = 0;
    record.className = "person";
    record.confidence = 0.95f;
    record.bboxLeft = 100;
    record.bboxTop = 100;
    record.bboxRight = 200;
    record.bboxBottom = 300;

    int64_t id = dao.insertDetection(record);
    TEST_ASSERT(id > 0, "插入单条检测记录");

    // 测试批量插入
    QVector<DetectionRecord> records;
    for (int i = 0; i < 10; i++) {
        DetectionRecord r;
        r.timestamp = now - (10 - i) * 1000;  // 每秒一条
        r.channel = i % 4;
        r.frameId = 1000 + i;
        r.classId = i % 3;
        r.className = (i % 3 == 0) ? "person" : ((i % 3 == 1) ? "helmet" : "car");
        r.confidence = 0.8f + (i % 10) * 0.02f;
        r.bboxLeft = 50 + i * 10;
        r.bboxTop = 50 + i * 10;
        r.bboxRight = 150 + i * 10;
        r.bboxBottom = 150 + i * 10;
        records.append(r);
    }
    int64_t inserted = dao.insertDetections(records);
    TEST_ASSERT(inserted == 10, "批量插入10条检测记录");

    // 测试查询：DetectionFilter 一次表达「时间档 + 通道 + 类别 + 分页」，
    // 取代原先 queryByTimeRange / queryByChannel / queryByClass 三个近亲函数。
    DetectionFilter f;
    f.startTime = now - 20000;
    f.endTime = now + 1000;

    auto results = dao.queryByFilter(f);
    TEST_ASSERT(results.size() >= 11, "按时间范围查询");
    TEST_ASSERT(dao.countByFilter(f) == results.size(), "计数与取行同一口径");

    f.channel = 0;
    TEST_ASSERT(dao.queryByFilter(f).size() > 0, "按通道查询");

    f.channel = -1;
    f.className = "person";
    TEST_ASSERT(dao.queryByFilter(f).size() > 0, "按类别查询");

    f.className.clear();
    auto facets = dao.classFacets(f.startTime, f.endTime);
    TEST_ASSERT(!facets.isEmpty(), "类别候选值来自数据库");

    // 测试统计
    int total = dao.totalCount(now - 20000, now + 1000);
    TEST_ASSERT(total >= 11, "总检测次数统计");

    auto classStats = dao.classStatistics(now - 20000, now + 1000);
    TEST_ASSERT(classStats.size() > 0, "类别统计");

    auto channelStats = dao.channelStatistics(now - 20000, now + 1000);
    TEST_ASSERT(channelStats.size() > 0, "通道统计");

    // 测试删除
    bool removed = dao.remove(id);
    TEST_ASSERT(removed, "删除单条记录");

    int cleared = dao.clearAll();
    TEST_ASSERT(cleared >= 10, "清空所有记录");
}

// ============================================================================
// 测试报警信息操作
// ============================================================================
void testAlarmOperations()
{
    std::cout << "\n=== 测试报警信息操作 ===" << std::endl;

    AlarmDAO dao;
    long now = QDateTime::currentMSecsSinceEpoch();

    // 测试插入
    AlarmRecord alarm;
    alarm.id = "test-alarm-001";
    alarm.timestamp = now;
    alarm.channel = 0;
    alarm.clsId = 0;
    alarm.className = "person";
    alarm.confidence = 0.9f;
    alarm.alarmType = "detection";
    alarm.alarmLevel = 2;
    alarm.status = "pending";
    alarm.imagePath = "/path/to/image.jpg";

    int64_t id = dao.insertAlarm(alarm);
    TEST_ASSERT(id > 0, "插入单条报警记录");

    // 测试批量插入
    QVector<AlarmRecord> alarms;
    for (int i = 0; i < 5; i++) {
        AlarmRecord a;
        a.id = QString("test-alarm-%1").arg(i + 10);
        a.timestamp = now - (5 - i) * 1000;
        a.channel = i % 4;
        a.clsId = i % 2;
        a.className = (i % 2 == 0) ? "person" : "helmet";
        a.confidence = 0.85f + i * 0.02f;
        a.alarmType = (i % 2 == 1) ? "fence" : "detection";
        a.alarmLevel = (i % 3) + 1;
        a.status = (i % 3 == 0) ? "rectified" : "pending";
        alarms.append(a);
    }
    int64_t inserted = dao.insertAlarms(alarms);
    TEST_ASSERT(inserted == 5, "批量插入5条报警记录");

    // 测试查询
    auto timeResults = dao.queryByTimeRange(now - 10000, now + 1000);
    TEST_ASSERT(timeResults.size() >= 6, "按时间范围查询报警");

    auto channelResults = dao.queryByChannel(0, now - 10000, now + 1000);
    TEST_ASSERT(channelResults.size() > 0, "按通道查询报警");

    auto unackResults = dao.queryUnacknowledged();
    TEST_ASSERT(unackResults.size() > 0, "查询未确认报警");

    // ---- 检索层：AlarmFilter 是报警列表唯一的取数入口，逐条边各测一次 ----
    // 先补一条带施工信息的，作为 worker/construction 两条过滤的唯一命中项。
    AlarmRecord withWorker;
    withWorker.id = "test-alarm-worker-1";
    withWorker.timestamp = now - 500;
    withWorker.channel = 2;
    withWorker.clsId = 0;
    withWorker.className = "person";
    withWorker.confidence = 0.77f;
    withWorker.alarmType = "detection";
    withWorker.alarmLevel = 2;
    withWorker.status = "pending";
    withWorker.workerName = "张三";
    withWorker.constructionJson =
        QStringLiteral("{\"construction_id\":\"SG-TEST-01\",\"worker_name\":\"张三\"}");
    TEST_ASSERT(dao.insertAlarm(withWorker) > 0, "插入带施工信息的报警");

    AlarmFilter af;
    af.startTime = now - 10000;
    af.endTime = now + 1000;
    const int all = dao.countByFilter(af);
    TEST_ASSERT(all >= 7, "AlarmFilter 计数");
    TEST_ASSERT(dao.queryByFilter(af).size() == all, "AlarmFilter 计数与取行同一口径");

    // 分页：limit 生效、offset 不重叠、两页合起来等于总数
    af.limit = 3;
    af.offset = 0;
    const auto page1 = dao.queryByFilter(af);
    af.offset = 3;
    const auto page2 = dao.queryByFilter(af);
    TEST_ASSERT(page1.size() == 3 && page2.size() == 3, "AlarmFilter 分页每页 3 条");
    TEST_ASSERT(page1.at(0).id != page2.at(0).id, "AlarmFilter 翻页取到不同的行");
    TEST_ASSERT(dao.countByFilter(af) == all, "钳页不改总数口径");
    af.limit = 500;
    af.offset = 0;

    // 围栏/类别互斥且并集等于全部（scope 用 alarm_type 的 fence 前缀区分）
    af.scope = AlarmFilter::ClassOnly;
    const int classOnly = dao.countByFilter(af);
    af.scope = AlarmFilter::FenceOnly;
    const int fenceOnly = dao.countByFilter(af);
    TEST_ASSERT(classOnly + fenceOnly == all, "类别报警与围栏报警不重不漏");

    // 角色：本批数据里的围栏都是历史值 "fence"（非 fence_out），所以只算闯入禁区
    af.fenceRole = AlarmFilter::RoleLeave;
    TEST_ASSERT(dao.countByFilter(af) == 0, "离岗角色只匹配 fence_out");
    af.fenceRole = AlarmFilter::RoleIntrusion;
    TEST_ASSERT(dao.countByFilter(af) == fenceOnly, "闯入角色匹配 fence 与 fence_in");
    af.fenceRole = AlarmFilter::RoleAll;
    af.scope = AlarmFilter::All;

    // 施工信息：负责人与施工计划各自一列（task_id 是 2026-11-04 随 B2 定案加的一等列），
    // 两条都是等值筛选——以前施工id 走 instr(construction_info,'"construction_id":"X"')，
    // 那是子串匹配，依赖 QJsonDocument::Compact 的序列化形状，键序/空格一变就静默查不到。
    af.workerName = "张三";
    TEST_ASSERT(dao.countByFilter(af) == 1, "按负责人筛选");
    af.workerName.clear();
    af.taskId = "SG-TEST-01";
    TEST_ASSERT(dao.countByFilter(af) == 1, "按施工计划(taskId)筛选");
    af.taskId = "SG-TEST-011";   // 前缀相同但不存在：等值判据，比 instr 更严
    TEST_ASSERT(dao.countByFilter(af) == 0, "施工id 不做前缀匹配");
    af.taskId.clear();

    // 候选值：列表页的下拉与表头菜单都从这三条来，取值必须带计数
    auto alarmClassFacets = dao.classFacets(now - 10000, now + 1000);
    TEST_ASSERT(!alarmClassFacets.isEmpty(), "报警类别候选值非空");
    auto workerFacets = dao.workerFacets(now - 10000, now + 1000);
    TEST_ASSERT(workerFacets.size() == 1 && workerFacets.at(0).first == "张三",
                "负责人候选值只含登录人");
    auto cidFacets = dao.taskIdFacets(now - 10000, now + 1000);
    TEST_ASSERT(cidFacets.size() == 1 && cidFacets.at(0).first == "SG-TEST-01",
                "taskId 候选值从 task_id 列出来");

    // 一键确认：把剩下未确认的一次改完，返回值必须与库里剩余 pending 对得上
    const int pendingBefore = dao.unacknowledgedCount();
    TEST_ASSERT(dao.acknowledgeAllPending() == pendingBefore, "全部确认影响行数与 pending 数一致");
    TEST_ASSERT(dao.unacknowledgedCount() == 0, "全部确认后不再有未确认");

    af.status = "false_alarm";
    TEST_ASSERT(dao.countByFilter(af) == 0, "误报状态可单独查");
    af.status.clear();
    TEST_ASSERT(dao.countByFilter(af) == all, "默认口径不含误报");

    // 测试确认
    bool acked = dao.updateStatus(alarm.id, "rectified");
    TEST_ASSERT(acked, "确认单条报警");

    // 测试标记误报
    bool marked = dao.markFalseAlarm(alarm.id);
    TEST_ASSERT(marked, "标记为误报");

    // 测试统计
    int total = dao.totalAlarmCount(now - 10000, now + 1000);
    TEST_ASSERT(total >= 6, "总报警次数统计");

    int unackCount = dao.unacknowledgedCount();
    TEST_ASSERT(unackCount >= 0, "未确认报警数量");

    auto classStats = dao.classStatistics(now - 10000, now + 1000);
    TEST_ASSERT(classStats.size() > 0, "报警类别统计");

    auto typeStats = dao.typeStatistics(now - 10000, now + 1000);
    TEST_ASSERT(typeStats.size() > 0, "报警类型统计");

    auto levelStats = dao.levelStatistics();
    TEST_ASSERT(levelStats.size() > 0, "报警级别统计");

    auto statusStats = dao.statusStatistics();
    TEST_ASSERT(statusStats.size() > 0, "报警状态统计");

    // 测试删除
    bool removed = dao.remove(alarm.id);
    TEST_ASSERT(removed, "删除单条报警");

    int cleared = dao.clearAll();
    TEST_ASSERT(cleared >= 5, "清空所有报警");
}

// ============================================================================
// 测试数据库维护
// ============================================================================
void testDatabaseMaintenance()
{
    std::cout << "\n=== 测试数据库维护 ===" << std::endl;

    // 测试数据库大小
    // 本测试用的是 ":memory:"，没有文件可 stat ⇒ databaseSize() 必然是 0。
    // 原来断言 >0 在这个库里永远失败，是测试自己的历史 bug，不是产品缺陷。
    int64_t size = DatabaseManager::instance().databaseSize();
    TEST_ASSERT(size == 0, "内存库没有落盘文件，大小按 0 报告");

    // 测试压缩
    bool vacuumed = DatabaseManager::instance().vacuum();
    TEST_ASSERT(vacuumed, "数据库压缩");

    // 测试数据清理
    int cleaned = DatabaseManager::instance().cleanOldDetections(30);
    TEST_ASSERT(cleaned >= 0, "清理过期检测数据");

    cleaned = DatabaseManager::instance().cleanOldAlarms(30);
    TEST_ASSERT(cleaned >= 0, "清理过期报警数据");
}

// ============================================================================
// main
// ============================================================================
// ============================================================================
// 测试老库升级：task_id 列补齐 + 回填 + 幂等（B2 定案，2026-11-04）
// ============================================================================
// 为什么这份用例必须跑**文件库**而不是继续用 :memory:：要验的是 ALTER TABLE + 事务 + WAL
// 凑在一起的行为，而 :memory: 每次 initialize 都是一份全新空库，永远走不到补列与回填那条路。
// 实测就是这条用例暴露了缺陷：回填的 commit 因"活动语句未收尾"失败，跟着的 rollback 把刚
// ALTER 出来的三列一起吞掉，老库升级当场废掉且只在日志里留两行。
void testLegacyTaskIdUpgrade(const QString &dbPath)
{
    std::cout << "\n=== 测试老库 task_id 升级与回填 ===" << std::endl;

    QFile::remove(dbPath);
    QFile::remove(dbPath + QStringLiteral("-wal"));
    QFile::remove(dbPath + QStringLiteral("-shm"));

    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "文件库初始化");

    AlarmDAO dao;
    const long now = QDateTime::currentMSecsSinceEpoch();

    AlarmRecord withTask;
    withTask.id = "legacy-task-a";
    withTask.timestamp = now;
    withTask.channel = 0;
    withTask.clsId = 0;
    withTask.className = "person";
    withTask.confidence = 0.9f;
    withTask.alarmType = "detection";
    withTask.alarmLevel = 3;
    withTask.status = "pending";
    withTask.workerName = QString::fromUtf8("李四");
    withTask.constructionJson = QString::fromUtf8(
        "{\"construction_id\":\"TASK-A\",\"worker_name\":\"李四\"}");
    // 故意不填 taskId：DAO 写入口必须从那段 JSON 兜底解出来，否则手工拼记录的老调用方
    // 会写进 NULL，而这行从此对 Pad 的 sync 永久不可见且零报错。
    TEST_ASSERT(dao.insertAlarm(withTask) > 0, "插入只带 construction_info 的老形状行");

    AlarmRecord noTask = withTask;
    noTask.id = "legacy-task-none";
    noTask.workerName.clear();
    noTask.constructionJson.clear();
    TEST_ASSERT(dao.insertAlarm(noTask) > 0, "插入未登录（无施工信息）的行");

    // 把两行都退回"补列之前"的形状。走自己的 QSqlQuery 而不是 DatabaseManager::executeSQL——
    // 后者是私有接口，测试不该为了取巧去扩大它的作用域。
    {
        QSqlQuery nullify(DatabaseManager::instance().database());
        TEST_ASSERT(nullify.exec("UPDATE alarms SET task_id = NULL"),
                    "模拟老行：task_id 置 NULL");
    }

    // 二次 initialize = 补列(幂等) + 回填，正是现场老库开机走的那条路
    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "同一份库二次 initialize");

    AlarmFilter byTask;
    byTask.taskId = "TASK-A";
    TEST_ASSERT(dao.countByFilter(byTask) == 1, "回填后按 taskId 等值能命中老行");

    QSqlDatabase db = DatabaseManager::instance().database();
    QSqlQuery left(db);
    TEST_ASSERT(left.exec("SELECT COUNT(*) FROM alarms WHERE task_id IS NULL")
                    && left.next() && left.value(0).toInt() == 0,
                "没有施工信息的行被写成空串：下次开机不再重扫（幂等的前提）");

    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "三次 initialize 无害");
    TEST_ASSERT(dao.countByFilter(byTask) == 1, "重复启动后仍能按 taskId 命中");

    db = DatabaseManager::instance().database();
    QSqlQuery shape(db);
    TEST_ASSERT(shape.exec("SELECT task_id FROM alarms WHERE id = 'legacy-task-none'")
                    && shape.next() && shape.value(0).toString().isEmpty()
                    && !shape.value(0).isNull(),
                "未登录行落的是空串而不是 NULL");

    // 关机时 AlarmManager::syncToDatabase 走的是 **批量** 路径
    // （`AlarmDAO::insertAlarms` → `INSERT OR REPLACE`），不是单条 insertAlarm。
    // 未登录行的 taskId/constructionJson 都是空 QString。Qt 把空 QString 当**无值**，
    // 绑进 SQL 就是 NULL——实测这一轮把刚收尾好的 1000 行 '' 全打回 NULL，
    // 于是每次开机都要重新扫+重新 UPDATE 那 1000 行，启动收尾等于白做。
    // INSERT 里的 `IFNULL(:task_id, '')` 只为这一条存在，所以这里必须重新走一次 DAO。
    noTask.id = "legacy-task-none";   // 同 id 覆盖，形状与 RAM 回写一致
    TEST_ASSERT(dao.insertAlarms({noTask}) > 0, "模拟关机时 RAM 窗口整批回写");

    db = DatabaseManager::instance().database();
    QSqlQuery afterWriteback(db);
    TEST_ASSERT(afterWriteback.exec("SELECT COUNT(*) FROM alarms WHERE task_id IS NULL")
                    && afterWriteback.next() && afterWriteback.value(0).toInt() == 0,
                "回写没有把已收尾的行打回 NULL");
    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "回写后再启动一次");
    QSqlQuery stillNull(DatabaseManager::instance().database());
    TEST_ASSERT(stillNull.exec("SELECT COUNT(*) FROM alarms WHERE task_id IS NULL")
                    && stillNull.next() && stillNull.value(0).toInt() == 0,
                "回写后的那次启动仍然扫到 0 行（幂等没被回写打破）");
}

// ============================================================================
// 老库补列与"缺列就停"的判据（§13.2 那六列 + dispose_person_id，2026-11-07）
// ============================================================================
// 钉两条方向相反的失效：
//   A. **误杀**：新加的"缺列 ⇒ 初始化失败"判据要是写错，板子开机直接没库，
//      比原来的静默丢报警严重得多，所以老库（真·上一代形状）必须一路走通。
//   B. **漏判**：AlarmDAO 的两条 INSERT 硬编码了新列名，补列没成功还继续跑
//      就是每一条报警都写不进库。少了判据 B，症状是"白天看着正常、关机全没"。
// A 用一份手工建的**老形状** alarms 表来走真实 ALTER 路径（:memory: 每次都是新库，
// 永远走不到补列，与 testLegacyTaskIdUpgrade 同一理由）。
void testLegacyProvenanceColumns(const QString &dbPath)
{
    std::cout << "\n=== 测试老库补列（§13.2 溯源列）与缺列判据 ===" << std::endl;

    QFile::remove(dbPath);
    QFile::remove(dbPath + QStringLiteral("-wal"));
    QFile::remove(dbPath + QStringLiteral("-shm"));

    // 先裸建一个连接，把 2026-11-06 之前那份表形状造出来：
    // 没有 worker_name/construction_info/task_id/dispose_person_id/rule/
    // model_version/fence_id/fence_version/region_id/dispose_option_id 这十列。
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("legacy_provenance_raw"));
        raw.setDatabaseName(dbPath);
        TEST_ASSERT(raw.open(), "老库：裸连接要能打开新建的库文件");
        QSqlQuery create(raw);
        TEST_ASSERT(create.exec(R"(
            CREATE TABLE alarms (
                id TEXT PRIMARY KEY,
                alarm_type TEXT NOT NULL,
                alarm_level INTEGER NOT NULL DEFAULT 3,
                alarm_time TEXT NOT NULL,
                channel INTEGER NOT NULL,
                class_id INTEGER NOT NULL,
                class_name TEXT NOT NULL,
                confidence REAL NOT NULL,
                image_path TEXT,
                video_path TEXT,
                status TEXT NOT NULL DEFAULT 'pending',
                dispose_result TEXT,
                dispose_user_id INTEGER,
                dispose_user_name TEXT,
                dispose_time TEXT,
                dispose_photo TEXT,
                remark TEXT,
                read_time TEXT,
                create_time TEXT NOT NULL,
                update_time TEXT
            ))"),
                    "老库：按上一代形状建表");
        QSqlQuery seed(raw);
        seed.prepare(QStringLiteral(
            "INSERT INTO alarms (id, alarm_type, alarm_level, alarm_time, channel, class_id,"
            " class_name, confidence, status, create_time)"
            " VALUES ('legacy-row-1','detection',3,'2026-11-06T09:00:00',0,0,'person',0.5,"
            " 'pending','2026-11-06T09:00:01')"));
        TEST_ASSERT(seed.exec(), "老库：塞一行历史报警");
        raw.close();
        QSqlDatabase::removeDatabase(QStringLiteral("legacy_provenance_raw"));
    }

    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath),
                "A) 老库初始化必须成功：新判据不能把补列成功的库误杀成失败");

    {
        QSqlDatabase db = DatabaseManager::instance().database();
        QSqlQuery pragma(db);
        TEST_ASSERT(pragma.exec(QStringLiteral("PRAGMA table_info(alarms)")), "老库：读列清单");
        QSet<QString> columns;
        while (pragma.next())
            columns.insert(pragma.value(1).toString().toLower());
        const char *needed[] = {"worker_name", "construction_info", "task_id",
                                "dispose_person_id", "rule", "model_version", "fence_id",
                                "fence_version", "region_id", "dispose_option_id"};
        bool allPresent = true;
        for (const char *name : needed) {
            if (!columns.contains(QString::fromLatin1(name))) {
                std::cout << "[INFO] 缺列: " << name << std::endl;
                allPresent = false;
            }
        }
        TEST_ASSERT(allPresent, "A) 十列都 ALTER 出来了");

        QSqlQuery survived(db);
        TEST_ASSERT(survived.exec(QStringLiteral("SELECT COUNT(*) FROM alarms"))
                        && survived.next() && survived.value(0).toInt() == 1,
                    "A) 历史行还在（补列没把老数据弄丢）");
    }

    // 真正的判据：DAO 的 INSERT 硬编码了这些列名，列没加上就是**每一条报警都插不进去**。
    AlarmDAO dao;
    AlarmRecord record;
    record.id = QStringLiteral("provenance-1");
    record.timestamp = QDateTime::currentMSecsSinceEpoch();
    record.channel = 0;
    record.clsId = 0;
    record.className = QStringLiteral("person");
    record.confidence = 0.7f;
    record.alarmType = QStringLiteral("fence_in");
    record.alarmLevel = 2;
    record.status = QStringLiteral("pending");
    record.rule = QStringLiteral("禁区闯入检测");
    record.modelVersion = QStringLiteral("yolo11n-ppe@2026-10-21");
    record.fenceId = QStringLiteral("NFENCE-1");
    record.fenceVersion = 3;
    record.regionId = QStringLiteral("RG-00000001");
    record.disposePersonId = QStringLiteral("05-0101");
    TEST_ASSERT(dao.insertAlarm(record) > 0, "B) 补列之后的 DAO 单条 INSERT 要成功");
    const AlarmRecord back = dao.queryById(QStringLiteral("provenance-1"));
    TEST_ASSERT(back.rule == record.rule && back.modelVersion == record.modelVersion
                    && back.fenceId == record.fenceId && back.fenceVersion == record.fenceVersion
                    && back.regionId == record.regionId
                    && back.disposePersonId == record.disposePersonId,
                "B) 六列写进去也读得回来（不是只建了个空壳列）");

    // 补列成功过的库再开机两次：新判据每次都读一次 PRAGMA，误杀就在这一步把板子
    // 变成"开机没有库"。这条是判据 A 的日常形态（现场每天开的那两次机），
    // 比一次性升级路径更值得钉。（DROP COLUMN 想造"缺列"造不出来：QSQLITE 插件
    // 内嵌的是 SQLite 3.31.1，DROP COLUMN 要 3.35+，实测 near "DROP": syntax error。）
    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "A) 二次 initialize 仍成功");
    TEST_ASSERT(DatabaseManager::instance().initialize(dbPath), "A) 三次 initialize 仍成功（幂等）");
    TEST_ASSERT(dao.queryById(QStringLiteral("provenance-1")).id == QStringLiteral("provenance-1"),
                "A) 重复启动没把刚插进去的溯源行弄丢");

    DatabaseManager::instance().close();
}

int main(int argc, char *argv[]){
    QCoreApplication app(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << "IDGE 数据库模块测试" << std::endl;
    std::cout << "========================================" << std::endl;

    // 运行测试
    testDatabaseInit();
    testDetectionOperations();
    testAlarmOperations();
    testDatabaseMaintenance();
    // 老库升级这条路径必须落在文件上（:memory: 每次都是全新空库，走不到 ALTER+回填），
    // 所以放在最后，用一个临时文件库单独跑一遍。
    testLegacyTaskIdUpgrade(QDir::currentPath() + QStringLiteral("/test_legacy_taskid.db"));
    // 老库补列与"缺列就停"的判据，同样必须落在文件库上（见函数头那段）
    testLegacyProvenanceColumns(QDir::currentPath() + QStringLiteral("/test_legacy_provenance.db"));

    // 输出结果
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试结果: " << testsPassed << " 通过, " << testsFailed << " 失败" << std::endl;
    std::cout << "========================================" << std::endl;

    // 关闭数据库
    DatabaseManager::instance().close();

    // 用例自带的临时库清掉：它落在构建目录里，留着会让人以为是第 N 份现场库
    const QString legacyDb = QDir::currentPath() + QStringLiteral("/test_legacy_taskid.db");
    QFile::remove(legacyDb);
    QFile::remove(legacyDb + QStringLiteral("-wal"));
    QFile::remove(legacyDb + QStringLiteral("-shm"));

    return testsFailed > 0 ? 1 : 0;
}
