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
    QString taskId;       // 施工计划 ID（v1 的 taskId；B2 定案＝库内 construction_info 的 construction_id，
                          // 2026-11-04 起有自己的一等列 task_id，筛选走等值而不是解 JSON）
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
    QVector<QPair<QString, int>> taskIdFacets(long startTime, long endTime,
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
     * @param taskId 非空时**在 SQL 里**按施工计划过滤（v1 的 taskId ＝ 库内 task_id 列；
     *        B2 于 2026-11-04 定案与 construction_id 同物）。空串＝不加这条条件
     *
     * 用 COALESCE(NULLIF(update_time,''), alarm_time)：早期版本建行时不写 update_time，
     * 老库里几万行是空串，直接按 update_time 过滤会把它们整批漏掉。
     * 时间文本仍是库内那种定宽无偏移格式（与 alarm_time 的字典序过滤同一套口径）。
     *
     * ⚠ `taskId` 这条过滤**必须在 SQL 里**，不能"先取 limit 行再在 C++ 里筛"：
     *   那是报告 §八 P1-2 的病灶——库里几万行历史时，升序取到的前 limit 行全是**别的应用**
     *   最早期的报警，筛完剩 0 条 → 游标回显 null → Pad 存 null → 下次仍是全量，
     *   该计划的报警**永远同步不到**且零报错（活锁）。下推之后 LIMIT 落在过滤之后，
     *   增量才会向前推进。
     * 判据与 buildFilterWhere() 的「按施工计划」那条逐字同一套：都是 `task_id = :task_id` 等值。
     * 2026-11-04 之前这里是 `instr(construction_info,'"construction_id":"X"')`（内嵌 SQLite
     * 3.31.1 没编 JSON1，json_extract 会 "no such function"），子串匹配依赖序列化形状且用不上
     * 索引，所以 B2 定案后把它升成了一等列 + idx_alarms_task_id。
     */
    QVector<AlarmRecord> queryUpdatedSince(const QString &sinceInternal, int limit = 500,
                                           const QString &taskId = QString());

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

    /**
     * @brief v1 §4.2 的处置：把一条报警写成终态 `disposed` 并落处置记录
     * @param measure       处置措施文本（对端从 §4.1 的选项 label 里选，落 dispose_result）
     * @param personId      处置人工号原文（v1 的 personId，例 "05-0101"）。落 dispose_person_id；
     *                      dispose_user_id 只有在它能整体解析成整数时才顺带写，否则保持原值不动
     * @param personName    处置人姓名（落 dispose_user_name）
     * @param remark        补充说明。**空串 = 不改这一列**（SQL 里是 COALESCE），
     *                      要清空请走 updateRemark()
     * @param photoPathsJson 整改照片路径的 JSON 数组文本（0~5 个），落 dispose_photo；
     *                      空串写 `[]` 而不是 NULL，读侧就只有一种形状要认
     * @param disposedAt    处置时间，**库内那套定宽无偏移文本**。空串 = 用本机当前时间
     * @param optionId      §4.1 选项的 id（1..4），落 dispose_option_id；0 = 没选/对端只给了
     *                      自由文本，这一列保持原值不动（措辞改了也不影响历史统计口径）
     * @return false = 没改成。含三种：库没开、id 不存在、已经是终态
     *          （WHERE 里挡掉 disposed/false_alarm，所以 §4.2 的 409/2003 不需要"先查再写"）
     *
     * ⚠ 调用方**必须**同时更新内存窗口里那一份同 id 记录，否则关机时的
     *   `syncToDatabase()` 会拿内存里 status 还是 pending 的旧副本 INSERT OR REPLACE 回来，
     *   处置就在第二天早上被静默撤销（和 task_id 回填被打回 NULL 是同一个病灶）。
     *   所以线上路径是 AlarmManager::disposeAlarmById()，不是直接调本函数。
     */
    bool dispose(const QString &id, const QString &measure, const QString &personId,
                 const QString &personName, const QString &remark,
                 const QString &photoPathsJson, const QString &disposedAt,
                 int optionId = 0);

    /**
     * @brief 标记误报
     * @param personId/personName/markedAt v1 §4.3 走这条时传；空串 = 该列不动（COALESCE）。
     *        UI 上那个「误报」按钮沿用旧口径全传空，所以它的行为与改动前逐字节相同。
     * @return false = 库没开 / id 不存在 / 已是终态（disposed 或 false_alarm 都不再改）
     */
    bool markFalseAlarm(const QString &id, const QString &remark = QString(),
                        const QString &personId = QString(),
                        const QString &personName = QString(),
                        const QString &markedAt = QString());

    /**
     * @brief 标记已读：read_time 保留**最早**值，并且每次都写 update_time
     * @param candidateTime 候选已读时间（库内定宽文本）。空串 = 用本机当前时间，
     *        也就是 v1 之前那条"板端看过就算已读"的老口径；
     *        非空 = §4.4 里 Pad 报上来的 readAt（Pad 第一次打开详情的时刻）。
     *        ⚠ 传进来了就**只信它**，不再与本机墙钟取小——Pad 的时钟由 hello_ack 校准过，
     *        而"边缘设备保留最早 readAt"要保的是**对端报的那个最早值**。
     */
    bool markRead(const QString &id, const QString &candidateTime = QString());

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
