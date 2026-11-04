#ifndef ALARM_DAO_H
#define ALARM_DAO_H

// ============================================================================
// alarm_dao.h - 报警信息数据访问对象
// ============================================================================
// 参考 wvp_safety_alarm 表结构
// ============================================================================

#include <QObject>
#include <QVector>
#include <QString>
#include <QPair>
#include "alarm_manager.h"

// ============================================================================
// AlarmFilter - 报警检索条件
// ============================================================================
// 为什么改成条件结构体而不是继续加 queryByXxx：
//   上一代 API 是「一个条件一个函数」，条件要组合就得嵌套，结果八个查询里只有
//   queryAll 有人调，界面退化成「读 AlarmManager 的 1000 行内存队列 + 本地过滤」。
//   实测运行时库 35383 条报警里，界面最多碰到 500 条 = 1.4%，其余在库里不可达。
//   一个结构体 + 一个 queryByFilter 之后，条件天然可组合，加字段只加一个成员。
//
// 语义约定（实现里不再重复说明）：
//   - 时间一律**毫秒**；startTime/endTime 都留 0 = 不限时间档
//   - channel < 0 = 全部通道
//   - 空字符串一律 = 不限该字段
//   - status 留空 = 默认口径「不含误报」（列表历来不展示 false_alarm）；
//     要看误报必须显式选中它，所以误报不会和"全部"混在一起，也不会看不见
//   - scope 是两张表分流的依据：围栏报警的 alarm_type 带 "fence" 前缀
//     （fence_in/fence_out/老库的 fence），和 recordFromQuery 还原 isFenceAlarm 同一口径
struct AlarmFilter {
    enum Scope { All, ClassOnly, FenceOnly };

    // 围栏表的第 5 列（围栏类型）只能由 alarm_type 决定，且判定必须和界面文案一致：
    // 列表把 "fence_out" 叫离岗、其它（含老库的 "fence"）叫闯入禁区，所以这里也只用这一条规则。
    enum FenceRole { RoleAll = 0, RoleIntrusion, RoleLeave };

    long startTime = 0;
    long endTime = 0;
    int channel = -1;
    QString className;
    QString status;
    QString workerName;
    QString constructionId;
    FenceRole fenceRole = RoleAll;
    Scope scope = All;
    int limit = 500;
    int offset = 0;
};

class AlarmDAO : public QObject
{
    Q_OBJECT
public:
    explicit AlarmDAO(QObject *parent = nullptr);

    // ========================================================================
    // 插入操作
    // ========================================================================

    // 插入单条报警记录
    int64_t insertAlarm(const AlarmRecord &alarm);

    // 批量插入报警记录
    int64_t insertAlarms(const QVector<AlarmRecord> &alarms);

    // ========================================================================
    // 查询操作
    // ========================================================================

    // 按条件组合查询（报警数据页的唯一查询入口，倒序 + 分页）
    QVector<AlarmRecord> queryByFilter(const AlarmFilter &filter);

    // 同一条件下的总条数（分页标签与"共 N 条"用，必须和 queryByFilter 同口径）
    int countByFilter(const AlarmFilter &filter);

    // 下面三个是筛选下拉的候选值 + 该值在当前时间档内的条数（点选式检索的底座，
    // 本机不做虚拟键盘，所以所有条件都得是"可点出来的"，不能是"可打出来的"）。
    // 只按时间档与 scope 收敛，**不**跟随其它已选条件：下拉里的候选与数字保持稳定，
    // 用户改一个条件不会让别的下拉只剩一项，也就不会把自己锁进空结果。
    QVector<QPair<QString, int>> classFacets(long startTime, long endTime,
                                             AlarmFilter::Scope scope = AlarmFilter::All);
    QVector<QPair<QString, int>> workerFacets(long startTime, long endTime,
                                              AlarmFilter::Scope scope = AlarmFilter::All);
    QVector<QPair<QString, int>> constructionFacets(long startTime, long endTime,
                                                    AlarmFilter::Scope scope = AlarmFilter::All);

    // 按时间范围查询
    QVector<AlarmRecord> queryByTimeRange(long startTime, long endTime,
                                           int limit = 1000, int offset = 0);

    // 按通道查询
    QVector<AlarmRecord> queryByChannel(int channel,
                                        long startTime, long endTime,
                                        int limit = 1000, int offset = 0);

    // 按类别查询
    QVector<AlarmRecord> queryByClass(const QString &className,
                                      long startTime, long endTime,
                                      int limit = 1000, int offset = 0);

    // 查询待处理报警 (status='pending')
    QVector<AlarmRecord> queryUnacknowledged(int limit = 1000);

    // 查询所有报警（按时间倒序）
    QVector<AlarmRecord> queryAll(int limit = 1000);

    // 按状态查询
    QVector<AlarmRecord> queryByStatus(const QString &status, int limit = 1000);

    // 按级别查询
    QVector<AlarmRecord> queryByLevel(int level, int limit = 1000);

    // 按ID查询
    AlarmRecord queryById(const QString &id);

    /**
     * @brief Pad 接口 v1 §3.8 的补漏查询：按**生效更新时间**升序返回 >= 游标 的行
     * @param sinceInternal 空串 = 全量（走没有 WHERE 的那条：Qt 把空 QString 绑成 NULL，
     *        `>= NULL` 恒不成立，绑成 "" 会一条都不回——首次全量就是这个形状）
     * @param limit 最多取几行
     * @param constructionId 非空时**在 SQL 里**按施工计划过滤（v1 的 taskId）
     *
     * 用 COALESCE(NULLIF(update_time,''), alarm_time)：早期版本建行时不写 update_time，
     * 老库里几万行是空串，直接按 update_time 过滤会把它们整批漏掉。
     * 时间文本仍是库内那种定宽无偏移格式（与 alarm_time 的字典序过滤同一套口径）。
     *
     * ⚠ `constructionId` 这条过滤**必须在 SQL 里**，不能"先取 limit 行再在 C++ 里筛"：
     *   那是报告 §八 P1-2 的病灶——库里几万行历史时，升序取到的前 limit 行全是**别的应用**
     *   最早期的报警，筛完剩 0 条 → 游标回显 null → Pad 存 null → 下次仍是全量，
     *   该计划的报警**永远同步不到**且零报错（活锁）。下推之后 LIMIT 落在过滤之后，
     *   增量才会向前推进。
     * 过滤表达式与 buildFilterWhere() 的「按施工id」那条逐字同一套（instr + 带引号的
     * 整段 needle）：本机 QSQLITE 插件内嵌 SQLite 3.31.1 且没编 JSON1，json_extract 会
     * "no such function"；LIKE 的 needle 里 %/_ 是通配符，而施工 id 与键名都带下划线，会过度匹配。
     */
    QVector<AlarmRecord> queryUpdatedSince(const QString &sinceInternal, int limit = 500,
                                           const QString &constructionId = QString());

    // ========================================================================
    // 更新操作
    // ========================================================================

    // 更新状态
    bool updateStatus(const QString &id, const QString &status);

    // 把当前所有未确认一次性置为已确认，返回受影响行数。
    // 为什么要有这条：报警列表现在是查库的（几万行可达），而「全部确认」原先是
    // 遍历内存里那 1000 条、逐条 UPDATE —— 实测 27858 条 pending 里它只改了 1000 条，
    // 剩下两万七在库里纹丝不动，按钮按下去数字都不带变的。
    int acknowledgeAllPending();

    // 处置报警
    bool dispose(const QString &id, const QString &result,
                 int userId, const QString &userName,
                 const QString &remark = QString());

    // 标记为误报
    bool markFalseAlarm(const QString &id, const QString &remark = QString());

    // 标记已读：read_time 保留**最早**值（重复调用不会把已读时间推后），
    // 并且每次都写 update_time —— 对端断线补漏按 update_time 做游标，漏写等于这条已读丢。
    bool markRead(const QString &id);

    // 更新备注
    bool updateRemark(const QString &id, const QString &remark);

    // 更新图片路径
    bool updateImagePath(const QString &id, const QString &imagePath);

    // 更新视频路径
    bool updateVideoPath(const QString &id, const QString &videoPath);

    // ========================================================================
    // 删除操作
    // ========================================================================

    // 删除单条报警
    bool remove(const QString &id);

    // 清空所有报警
    int clearAll();

    // 清理N天前的报警数据
    int cleanOldAlarms(int daysToKeep);

    // ========================================================================
    // 统计操作
    // ========================================================================

    // 总报警次数
    int totalAlarmCount(long startTime = 0, long endTime = 0);

    // 待处理报警数量
    int unacknowledgedCount();

    // 按通道统计
    QMap<int, int> channelStatistics(long startTime = 0, long endTime = 0);

    // 按类别统计
    QMap<QString, int> classStatistics(long startTime = 0, long endTime = 0);

    // 按报警类型统计
    QMap<QString, int> typeStatistics(long startTime = 0, long endTime = 0);

    // 按报警级别统计
    QMap<int, int> levelStatistics();

    // 按状态统计
    QMap<QString, int> statusStatistics();

private:
    // 从查询结果构建 AlarmRecord
    AlarmRecord recordFromQuery(class QSqlQuery &query);
};

#endif // ALARM_DAO_H
