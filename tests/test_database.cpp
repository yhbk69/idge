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

    // 施工信息：负责人走独立列，施工id 走 instr(construction_info, '"construction_id":"X"')
    // ——Qt 内建的 SQLite 没有 JSON1，json_extract 在这里根本不可用。
    af.workerName = "张三";
    TEST_ASSERT(dao.countByFilter(af) == 1, "按负责人筛选");
    af.workerName.clear();
    af.constructionId = "SG-TEST-01";
    TEST_ASSERT(dao.countByFilter(af) == 1, "按施工id筛选");
    af.constructionId = "SG-TEST-011";   // 前缀相同但不存在：instr 是精确子串，不是 LIKE
    TEST_ASSERT(dao.countByFilter(af) == 0, "施工id 不做前缀匹配");
    af.constructionId.clear();

    // 候选值：列表页的下拉与表头菜单都从这三条来，取值必须带计数
    auto alarmClassFacets = dao.classFacets(now - 10000, now + 1000);
    TEST_ASSERT(!alarmClassFacets.isEmpty(), "报警类别候选值非空");
    auto workerFacets = dao.workerFacets(now - 10000, now + 1000);
    TEST_ASSERT(workerFacets.size() == 1 && workerFacets.at(0).first == "张三",
                "负责人候选值只含登录人");
    auto cidFacets = dao.constructionFacets(now - 10000, now + 1000);
    TEST_ASSERT(cidFacets.size() == 1 && cidFacets.at(0).first == "SG-TEST-01",
                "施工id 候选值从 construction_info 解析出来");

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
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    std::cout << "========================================" << std::endl;
    std::cout << "IDGE 数据库模块测试" << std::endl;
    std::cout << "========================================" << std::endl;

    // 运行测试
    testDatabaseInit();
    testDetectionOperations();
    testAlarmOperations();
    testDatabaseMaintenance();

    // 输出结果
    std::cout << "\n========================================" << std::endl;
    std::cout << "测试结果: " << testsPassed << " 通过, " << testsFailed << " 失败" << std::endl;
    std::cout << "========================================" << std::endl;

    // 关闭数据库
    DatabaseManager::instance().close();

    return testsFailed > 0 ? 1 : 0;
}
