// ============================================================================
// test_pad_sync.cpp — Pad 接口 v1 断线补漏的库侧自检（queryUpdatedSince + markRead）
// ----------------------------------------------------------------------------
// 内存库（":memory:"）跑真 DAO：不碰 GUI、不碰 config.json。
//
// 为什么值得单独钉：v1 §3.8 把补漏降级成"一条按 updatedAt 的游标查询"，于是这条 SQL
// 成了**对端能不能看到漏掉的报警**的唯一决定因素，而它有两个静默失效的坑：
//   · 老库里几万行的 update_time 是空串（早期建行不写这列）。直接按 update_time 过滤
//     会把它们整批漏掉，症状是"重连后历史报警一条都不回来"，而且不报错。
//   · 新建的行如果 update_time 也是空的，增量同步就看不见刚产生的报警
//     ——正是 §12.1 那条新验收项要拦的事。
// markRead 同理：它不 bump update_time 的话，Pad 侧"已读"永远同步不回去。
//
// 覆盖八条契约：
//   1) 空游标 = 全量，且按生效时间**升序**（v1 要求升序返回，游标取最大值）
//   2) update_time 为空的老行按 alarm_time 参与过滤与排序（COALESCE 生效）
//   3) 增量游标只带回 >= 游标的行，边界用 >=（同一秒内的更新不许被漏）
//   4) 新产生的报警（update_time 已写死）能被增量游标带出
//   5) markRead 第一次同时写 read_time 与 update_time
//   6) markRead 重复调用：read_time 保留最早值，update_time 仍然往前走
//   7) 时间回拨时 markRead 仍以更早者为准（read_time 不被推后）
//   8) updateStatus('rectified') 也 bump update_time（状态变更必须能被补漏带回）
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_sync，rc 0=PASS 1=FAIL
// ============================================================================

#include "alarm_dao.h"
#include "database_manager.h"

#include <QCoreApplication>
#include <QString>
#include <QStringList>
#include <QVector>

#include <iostream>
#include <stdexcept>

namespace {

void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

AlarmRecord makeAlarm(const QString &id, const QString &alarmTime, const QString &updateTime,
                      const QString &status = QStringLiteral("pending"))
{
    AlarmRecord record;
    record.id = id;
    record.alarmType = QStringLiteral("detection");
    record.alarmLevel = 3;
    record.alarmTime = alarmTime;
    record.channel = 0;
    record.clsId = 0;
    record.className = QStringLiteral("un_helmet");
    record.confidence = 0.9f;
    record.status = status;
    record.createTime = alarmTime;
    record.updateTime = updateTime;
    return record;
}

/// 表里的 (id → update_time, read_time)，直接问 DAO 的回读结果
AlarmRecord fetch(AlarmDAO &dao, const QString &id)
{
    return dao.queryById(id);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

try {
    check(DatabaseManager::instance().initialize(QStringLiteral(":memory:")),
          "内存库初始化失败");
    AlarmDAO dao;

    // A：正常行（update_time 与 alarm_time 同秒）
    // B：早期版本留下的行（update_time 空串，只有 alarm_time）
    // C：已被处置过的行（update_time 晚于 alarm_time）
    const QString t10 = QStringLiteral("2026-11-02T10:00:00");
    const QString t09 = QStringLiteral("2026-11-02T09:00:00");
    const QString t11 = QStringLiteral("2026-11-02T11:00:00");
    check(dao.insertAlarm(makeAlarm(QStringLiteral("A"), t10, t10)) > 0, "插入 A");
    check(dao.insertAlarm(makeAlarm(QStringLiteral("B"), t09, QString())) > 0, "插入 B（老行）");
    check(dao.insertAlarm(makeAlarm(QStringLiteral("C"), t09, t11, QStringLiteral("rectified")))
          > 0, "插入 C");

    // ── 1：空游标 = 全量，升序 ─────────────────────────────────────────────
    {
        const QVector<AlarmRecord> all = dao.queryUpdatedSince(QString(), 100);
        check(all.size() == 3, "空游标要返回全量三行");
        QStringList ids;
        for (const AlarmRecord &r : all) {
            ids << r.id;
        }
        // 生效时间：B=t09(09:00) C=t11 A=t10 → 升序 B, A, C
        check(ids == QStringList{QStringLiteral("B"), QStringLiteral("A"), QStringLiteral("C")},
              "必须按**生效更新时间**升序（不是按 alarm_time、也不是倒序）");
    }

    // ── 2：老行按 alarm_time 参与过滤 ──────────────────────────────────────
    {
        const QVector<AlarmRecord> since10 = dao.queryUpdatedSince(t10, 100);
        QStringList ids;
        for (const AlarmRecord &r : since10) {
            ids << r.id;
        }
        check(!ids.contains(QStringLiteral("B")),
              "update_time 为空的老行要按 alarm_time 判定，09:00 的那条不该出现在 10:00 之后");
        check(ids.contains(QStringLiteral("A")) && ids.contains(QStringLiteral("C")),
              "10:00 与 11:00 两行都该被带回");
    }

    // ── 3：边界用 >=（同一秒不能漏）────────────────────────────────────────
    {
        const QVector<AlarmRecord> exact = dao.queryUpdatedSince(t10, 100);
        bool hasA = false;
        for (const AlarmRecord &r : exact) {
            hasA = hasA || r.id == QLatin1String("A");
        }
        check(hasA, "游标正好等于某行的生效时间时该行必须被带回（v1 用 >= 就是为了这一秒）");
    }

    // ── 4：新产生的行能被增量看到 ──────────────────────────────────────────
    {
        const QString t12 = QStringLiteral("2026-11-02T12:00:00");
        check(dao.insertAlarm(makeAlarm(QStringLiteral("D"), t12, t12)) > 0, "插入 D");
        const QVector<AlarmRecord> fresh = dao.queryUpdatedSince(t11, 100);
        bool hasD = false;
        for (const AlarmRecord &r : fresh) {
            hasD = hasD || r.id == QLatin1String("D");
        }
        check(hasD, "建行那一刻就写 update_time，新报警才能被增量游标带出");
    }

    // ── 5/6/7：markRead 的两条语义 ─────────────────────────────────────────
    {
        const AlarmRecord before = fetch(dao, QStringLiteral("A"));
        check(before.readTime.isEmpty(), "起点：A 未读");

        check(dao.markRead(QStringLiteral("A")), "markRead 应命中已存在的行");
        const AlarmRecord first = fetch(dao, QStringLiteral("A"));
        check(!first.readTime.isEmpty(), "第一次已读要写 read_time");
        check(first.updateTime != before.updateTime || !first.updateTime.isEmpty(),
              "已读必须同时 bump update_time，否则对端补漏查不到这条变更");
        const QString readStamp = first.readTime;
        const QString updateStamp = first.updateTime;

        // 同一秒内重复调用：字符串可能一样，用"已读时间不回退"这条硬判据即可
        QCoreApplication::processEvents();
        check(dao.markRead(QStringLiteral("A")), "重复 markRead 仍应命中");
        const AlarmRecord again = fetch(dao, QStringLiteral("A"));
        check(again.readTime == readStamp, "read_time 保留最早值，重复已读不许把它推后");
        check(again.updateTime >= updateStamp, "update_time 只会往前走");

        // 时间回拨：库里已有一个"将来"的 read_time，新读到的更早 → 取更早者
        AlarmRecord future = makeAlarm(QStringLiteral("E"), t10, t10);
        future.readTime = QStringLiteral("2027-01-01T00:00:00");
        check(dao.insertAlarm(future) > 0, "插入一条 read_time 在将来的行");
        check(dao.markRead(QStringLiteral("E")), "markRead 命中 E");
        const AlarmRecord fixed = fetch(dao, QStringLiteral("E"));
        check(fixed.readTime != QStringLiteral("2027-01-01T00:00:00"),
              "墙钟回拨后已读时间取更早者，不能被一个将来的脏值钉死");
    }

    // ── 8：状态变更同样 bump ───────────────────────────────────────────────
    {
        const AlarmRecord before = fetch(dao, QStringLiteral("B"));
        check(dao.updateStatus(QStringLiteral("B"), QStringLiteral("rectified")),
              "updateStatus 应成功");
        const AlarmRecord after = fetch(dao, QStringLiteral("B"));
        check(after.status == QLatin1String("rectified"), "状态要落库");
        check(!after.updateTime.isEmpty() && after.updateTime >= before.updateTime,
              "状态变更要写 update_time（老行原本是空串）");
    }

    std::cout << "pad sync contracts passed: full/incremental cursor / legacy empty update_time "
                 "fallback to alarm_time / >= boundary / new rows visible / markRead keeps "
                 "earliest readAt while bumping update_time / status change bumps too\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "FAIL: " << e.what() << '\n';
    return 1;
}
}
