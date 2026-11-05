// ============================================================================
// alarm_dao.cpp - 报警信息数据访问对象实现
// ============================================================================
// 职责：
//   封装 alarms 表的所有数据库操作（CRUD + 统计），
//   提供给 AlarmManager 调用。
//
// 设计要点：
//   1. 所有操作通过 DatabaseManager 获取共享的 QSqlDatabase 连接
//   2. 批量插入使用事务（db.transaction/commit）提高性能
//   3. 查询结果通过 recordFromQuery() 统一转换为 AlarmRecord 结构体
//   4. 时间字段使用 ISO8601 格式字符串存储（SQLite 无原生时间类型）
//
// 参考 wvp_safety_alarm 表结构
// ============================================================================

#include "alarm_dao.h"
#include "runtime_paths.h"
#include "database_manager.h"
#include "construction_session.h"   // 施工id 的键名常量，别在这儿再抄一遍字面量
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QUuid>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

/**
 * @class AlarmDAO
 * @brief alarms 表的数据访问对象：无状态，每个方法各自取一次连接
 *
 * 线程安全：本类不持有连接、也不加锁。DatabaseManager 给每个线程返回**它自己的**
 * QSqlDatabase 连接，所以后台线程（ClipWriter 回填、Pad 同步、GUI 查询）各 new 一个
 * AlarmDAO 并发调用是安全的；同一个 AlarmDAO 实例跨线程用则不保证。
 *
 * 失败口径（所有方法一致，调用方按类型判）：
 *   库没开 → 写返回 -1/0/false、查询返回空表；SQL 执行失败 → 同上，并 qWarning 一条；
 *   UPDATE/DELETE 的 false **同时**包含"SQL 失败"和"没有行被影响（id 不存在）"两种情况，
 *   调用方要区分只能自己先查（AlarmManager 现在不区分，只记一条日志）。
 */
AlarmDAO::AlarmDAO(QObject *parent)
    : QObject(parent)
{
}

// ============================================================================
// 生成 UUID
// ============================================================================
// 32 位十六进制、不带连字符与大括号：与 AlarmManager::makeAlarmRecord 生成 id 的写法
// 必须逐字一致，否则内存里的 id 和库里的对不上，片段回填与 Pad 状态推送都会认不出行。
static QString generateUuid()
{
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// ============================================================================
// 获取当前时间字符串 (ISO8601格式)
// ============================================================================
// Qt::ISODate 在"当前时刻无小数秒"时产出定宽 19 字符 "yyyy-MM-ddTHH:mm:ss"（本地时间、
// 不带偏移）。这个**定宽**性质是检索层的地基：alarm_time/update_time 的全部范围过滤都靠
// 文本字典序 BETWEEN（见 buildFilterWhere），一旦格式变成带毫秒或带 +08:00，
// 新行与老行混着比就会静默错序，而不是报错。改这里之前先读 src/biz/db/README.md。
static QString currentDateTimeStr()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
}

// ============================================================================
// 写库时这一行的施工计划身份（v1 的 taskId；扫码载荷里是 plan.taskId）
// ============================================================================
// 记录自带的 taskId 优先；没填就从 construction_info 解（新形状读 plan.taskId，
// 历史行回落 construction_id）。兜底不是多余：测试与老调用方
// 是手工拼 AlarmRecord 的，只喂 constructionJson 不填新字段，少这一步就写成 NULL，
// 而现在所有过滤只看 task_id —— 症状是那一行对 Pad 永久不可见，而且零报错。
// ⚠ 这条判据**不能**保证返回非 null：空 constructionJson 解出来就是空 QString，
// 而 Qt 把空 QString 当**无值**绑进 SQL，落库是 NULL 而不是 ''。所以这一列的
// "永不为 NULL"不变式由两条 INSERT 里的 `IFNULL(:task_id, '')` 兜住，不靠这里的返回值。
// 为什么必须在 SQL 侧兜：AlarmManager 每次关机把 RAM 窗口（1000 条）整批回写，
// 这 1000 行未登录老行的 taskId 是空串 → 绑成 NULL → 把启动回填收尾写好的 '' 打回 NULL，
// 于是回填每一开机都要重新扫并重新 UPDATE 那 1000 行（实测 `0 from JSON + 1000 sealed empty`）。
static QString taskIdForInsert(const AlarmRecord &alarm)
{
    return alarm.taskId.isEmpty()
               ? session::ConstructionInfo::taskIdFromJsonText(alarm.constructionJson)
               : alarm.taskId;
}

// ============================================================================
// insertAlarm: 插入单条报警记录
// ============================================================================
// 流程：
//   1. 获取数据库连接（通过 DatabaseManager 单例）
//   2. 如果 alarm.id 为空，自动生成 UUID 作为主键
//   3. 准备 SQL INSERT 语句，绑定 20 个字段参数
//   4. 执行插入，返回 1 表示成功，-1 表示失败
//
// 注意：
//   - SQLite 不支持 lastInsertId for TEXT primary key，所以固定返回 1
//   - createTime 如果为空，自动填充当前时间
// ============================================================================
int64_t AlarmDAO::insertAlarm(const AlarmRecord &alarm)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        return -1;
    }

    // 如果id为空，生成UUID
    QString id = alarm.id.isEmpty() ? generateUuid() : alarm.id;
    QString createTime = alarm.createTime.isEmpty() ? currentDateTimeStr() : alarm.createTime;

    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO alarms (id, alarm_type, alarm_level, alarm_time, channel,
                           class_id, class_name, confidence, image_path, video_path,
                           status, dispose_result, dispose_user_id, dispose_user_name,
                           dispose_time, dispose_photo, remark, read_time,
                           create_time, update_time, worker_name, construction_info,
                           task_id)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time, :worker_name, :construction_info,
                IFNULL(:task_id, ''))
    )");

    query.bindValue(":id", id);
    query.bindValue(":alarm_type", alarm.alarmType);
    query.bindValue(":alarm_level", alarm.alarmLevel);
    query.bindValue(":alarm_time", alarm.alarmTime.isEmpty() ?
                     QDateTime::fromMSecsSinceEpoch(alarm.timestamp).toString(Qt::ISODate) :
                     alarm.alarmTime);
    query.bindValue(":channel", alarm.channel);
    query.bindValue(":class_id", alarm.clsId);
    query.bindValue(":class_name", alarm.className);
    query.bindValue(":confidence", static_cast<double>(alarm.confidence));
    query.bindValue(":image_path", alarm.imagePath);
    query.bindValue(":video_path", alarm.videoPath);
    query.bindValue(":status", alarm.status);
    query.bindValue(":dispose_result", alarm.disposeResult);
    query.bindValue(":dispose_user_id", alarm.disposeUserId);
    query.bindValue(":dispose_user_name", alarm.disposeUserName);
    query.bindValue(":dispose_time", alarm.disposeTime);
    query.bindValue(":dispose_photo", alarm.disposePhoto);
    query.bindValue(":remark", alarm.remark);
    query.bindValue(":read_time", alarm.readTime);
    query.bindValue(":create_time", createTime);
    query.bindValue(":update_time", alarm.updateTime);
    // 施工信息：未登录时为空串，入库即 NULL 语义（显示侧统一成 "—"）
    query.bindValue(":worker_name", alarm.workerName);
    query.bindValue(":construction_info", alarm.constructionJson);
    query.bindValue(":task_id", taskIdForInsert(alarm));

    if (!query.exec()) {
        qWarning() << "Insert alarm failed:" << query.lastError().text();
        return -1;
    }

    return 1;  // SQLite不支持lastInsertId for TEXT primary key
}

// ============================================================================
// insertAlarms: 批量插入报警记录（使用事务，性能优化）
// ============================================================================
// 流程：
//   1. 获取数据库连接
//   2. 开启事务（db.transaction()）—— 批量插入必须用事务，否则每条都写磁盘
//   3. 遍历所有报警记录，逐条执行 INSERT
//   4. 提交事务（db.commit()）—— 一次性写入磁盘
//   5. 如果提交失败，回滚事务（db.rollback()）
//
// 性能说明：
//   - 不用事务：每条 INSERT 都触发一次 fsync → 100条 = 100次磁盘IO
//   - 使用事务：100条 INSERT + 1次 fsync → 提速 50-100 倍
//
// 返回：实际成功插入的记录数
// ============================================================================
/**
 * @brief 批量插入报警（一条事务包住整批）
 * @return 实际执行成功的行数；**提交失败时返回 0**（此时整批已 rollback，
 *         前面累加的 inserted 一起作废，调用方不必再补写）
 *
 * 用 INSERT OR REPLACE 而不是 INSERT：同 id 重写覆盖（重试/回灌同一条报警时不炸主键）。
 * ⚠ 代价是 REPLACE 语义为"删掉旧行再插新行"，所以**入参必须是完整记录**——
 * 只填几个字段会把 dispose_*、read_time、remark 一起冲成空值。当前唯一写入口是
 * AlarmManager::storeAndNotify（刚产生的报警，那些列本来为空），所以安全；
 * 新增调用方若拿的是从库里读回的记录，走的是 recordFromQuery，字段是全的。
 * 空列表直接返回 0，不开事务（省一次空 fsync）。
 */
int64_t AlarmDAO::insertAlarms(const QVector<AlarmRecord> &alarms)
{
    if (alarms.isEmpty()) {
        return 0;
    }

    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        return 0;
    }

    int64_t inserted = 0;
    db.transaction();

    QSqlQuery query(db);
    query.prepare(R"(
        INSERT OR REPLACE INTO alarms (id, alarm_type, alarm_level, alarm_time, channel,
                           class_id, class_name, confidence, image_path, video_path,
                           status, dispose_result, dispose_user_id, dispose_user_name,
                           dispose_time, dispose_photo, remark, read_time,
                           create_time, update_time, worker_name, construction_info,
                           task_id)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time, :worker_name, :construction_info,
                IFNULL(:task_id, ''))
    )");

    for (const AlarmRecord &alarm : alarms) {
        QString id = alarm.id.isEmpty() ? generateUuid() : alarm.id;
        QString createTime = alarm.createTime.isEmpty() ? currentDateTimeStr() : alarm.createTime;

        query.bindValue(":id", id);
        query.bindValue(":alarm_type", alarm.alarmType);
        query.bindValue(":alarm_level", alarm.alarmLevel);
        query.bindValue(":alarm_time", alarm.alarmTime.isEmpty() ?
                         QDateTime::fromMSecsSinceEpoch(alarm.timestamp).toString(Qt::ISODate) :
                         alarm.alarmTime);
        query.bindValue(":channel", alarm.channel);
        query.bindValue(":class_id", alarm.clsId);
        query.bindValue(":class_name", alarm.className);
        query.bindValue(":confidence", static_cast<double>(alarm.confidence));
        query.bindValue(":image_path", alarm.imagePath);
        query.bindValue(":video_path", alarm.videoPath);
        query.bindValue(":status", alarm.status);
        query.bindValue(":dispose_result", alarm.disposeResult);
        query.bindValue(":dispose_user_id", alarm.disposeUserId);
        query.bindValue(":dispose_user_name", alarm.disposeUserName);
        query.bindValue(":dispose_time", alarm.disposeTime);
        query.bindValue(":dispose_photo", alarm.disposePhoto);
        query.bindValue(":remark", alarm.remark);
        query.bindValue(":read_time", alarm.readTime);
        query.bindValue(":create_time", createTime);
        query.bindValue(":update_time", alarm.updateTime);
        query.bindValue(":worker_name", alarm.workerName);
        query.bindValue(":construction_info", alarm.constructionJson);
        query.bindValue(":task_id", taskIdForInsert(alarm));

        if (query.exec()) {
            inserted++;
        } else {
            qWarning() << "Insert alarm failed:" << query.lastError().text();
        }
    }

    if (!db.commit()) {
        qWarning() << "Transaction commit failed:" << db.lastError().text();
        db.rollback();
        return 0;
    }

    return inserted;
}

// ============================================================================
// 查询操作
// ============================================================================
// 所有查询方法的共同特点：
//   1. 通过 DatabaseManager 获取共享的 QSqlDatabase 连接
//   2. 结果按 alarm_time DESC 倒序排列（最新在前）
//   3. 支持分页：limit 限制返回数量，offset 偏移量
//   4. 返回 QVector<AlarmRecord>，查询失败返回空列表
// ============================================================================

/**
 * @brief 按时间范围查询报警记录
 *
 * @param startTime: 起始时间（毫秒时间戳，如 QDateTime::currentMSecsSinceEpoch()）
 * @param endTime:   结束时间（毫秒时间戳）
 * @param limit:     最大返回数量（默认 1000）
 * @param offset:    分页偏移量（默认 0，用于翻页）
 * @return: 报警记录列表，按时间倒序，无结果返回空列表
 */
QVector<AlarmRecord> AlarmDAO::queryByTimeRange(long startTime, long endTime,
                                                 int limit, int offset)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    // 转换时间戳为ISO格式
    QString startStr = QDateTime::fromMSecsSinceEpoch(startTime).toString(Qt::ISODate);
    QString endStr = QDateTime::fromMSecsSinceEpoch(endTime).toString(Qt::ISODate);

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE alarm_time BETWEEN :start AND :end
        ORDER BY alarm_time DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":start", startStr);
    query.bindValue(":end", endStr);
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

/**
 * @brief 按通道查询报警记录
 *
 * @param channel:   通道号（0-3，对应视频通道 1-4）
 * @param startTime: 起始时间（毫秒时间戳）
 * @param endTime:   结束时间（毫秒时间戳）
 * @param limit:     最大返回数量（默认 1000）
 * @param offset:    分页偏移量（默认 0）
 * @return: 该通道的报警记录列表，按时间倒序
 */
QVector<AlarmRecord> AlarmDAO::queryByChannel(int channel,
                                               long startTime, long endTime,
                                               int limit, int offset)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QString startStr = QDateTime::fromMSecsSinceEpoch(startTime).toString(Qt::ISODate);
    QString endStr = QDateTime::fromMSecsSinceEpoch(endTime).toString(Qt::ISODate);

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE channel = :channel AND alarm_time BETWEEN :start AND :end
        ORDER BY alarm_time DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":channel", channel);
    query.bindValue(":start", startStr);
    query.bindValue(":end", endStr);
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

/**
 * @brief 按类别查询报警记录
 *
 * @param className: 类别名称（如 "person"、"helmet"、"vest"）
 * @param startTime: 起始时间（毫秒时间戳）
 * @param endTime:   结束时间（毫秒时间戳）
 * @param limit:     最大返回数量（默认 1000）
 * @param offset:    分页偏移量（默认 0）
 * @return: 该类别的报警记录列表，按时间倒序
 */
QVector<AlarmRecord> AlarmDAO::queryByClass(const QString &className,
                                            long startTime, long endTime,
                                            int limit, int offset)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QString startStr = QDateTime::fromMSecsSinceEpoch(startTime).toString(Qt::ISODate);
    QString endStr = QDateTime::fromMSecsSinceEpoch(endTime).toString(Qt::ISODate);

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE class_name = :class_name AND alarm_time BETWEEN :start AND :end
        ORDER BY alarm_time DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":class_name", className);
    query.bindValue(":start", startStr);
    query.bindValue(":end", endStr);
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

/**
 * @brief 查询未确认的报警记录（status='pending'）
 *
 * @param limit: 最大返回数量（默认 1000）
 * @return: 未确认的报警记录列表，按时间倒序
 */
QVector<AlarmRecord> AlarmDAO::queryUnacknowledged(int limit)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE status = 'pending'
        ORDER BY alarm_time DESC
        LIMIT :limit
    )");
    query.bindValue(":limit", limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

// ============================================================================
// queryAll: 查询所有报警（按时间倒序）
// ============================================================================
// 用于程序启动时加载历史报警，重启后用户可查看所有状态的报警记录
// ============================================================================
QVector<AlarmRecord> AlarmDAO::queryAll(int limit)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        ORDER BY alarm_time DESC
        LIMIT :limit
    )");
    query.bindValue(":limit", limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

// ============================================================================
// queryByFilter / countByFilter / *Facets: 组合条件检索
// ============================================================================
//
// WHERE 由 buildFilterWhere 统一生成，queryByFilter 与 countByFilter 共用它 ——
// 「列表里的条数」和「列表里的行」只要有一次口径不同，分页标签就会开始骗人，
// 所以这两条 SQL 不许各自拼一遍。
// ============================================================================
namespace {

struct FilterSql {
    QString where;                             // 永远以 "WHERE " 开头，无条件时是 WHERE 1=1
    QVector<QPair<QString, QVariant>> binds;   // 具名占位符 → 值
};

QString isoOf(long msecs)
{
    return QDateTime::fromMSecsSinceEpoch(msecs).toString(Qt::ISODate);
}

// 把 AlarmFilter 翻成 WHERE 片段。
// 铁律：条件文本里不出现任何取值，取值一律走 bindValue。列名全是写死的字面量，
// 所以这里不存在注入面；将来加条件也得照这个形状加，别拼字符串。
FilterSql buildFilterWhere(const AlarmFilter &f)
{
    FilterSql out;
    QStringList parts;

    auto eq = [&parts, &out](const char *column, const char *name, const QVariant &value) {
        parts << QStringLiteral("%1 = :%2").arg(QLatin1String(column), QLatin1String(name));
        out.binds.append(qMakePair(QString::fromLatin1(name), value));
    };

    if (f.startTime > 0 && f.endTime > 0) {
        // alarm_time 是定宽 ISO 文本（"yyyy-MM-ddTHH:mm:ss"，实测 35383 行全是 19 字符、
        // 分隔符统一是 T、无小数秒），所以字典序 == 时间序，BETWEEN 可以直接比文本，
        // 并且吃 idx_alarms_time。写入端 currentDateTimeStr() 用的就是 Qt::ISODate，
        // 这条等价关系的前提在写入口上，不在查询里。
        parts << QStringLiteral("alarm_time BETWEEN :t_start AND :t_end");
        out.binds.append(qMakePair(QStringLiteral("t_start"), isoOf(f.startTime)));
        out.binds.append(qMakePair(QStringLiteral("t_end"), isoOf(f.endTime)));
    }
    if (f.channel >= 0) eq("channel", "channel", f.channel);
    if (!f.className.isEmpty()) eq("class_name", "class_name", f.className);
    if (!f.workerName.isEmpty()) eq("worker_name", "worker_name", f.workerName);

    if (f.status.isEmpty()) {
        parts << QStringLiteral("status <> 'false_alarm'");
    } else {
        eq("status", "status", f.status);
    }

    if (!f.taskId.isEmpty()) {
        // 等值判据：task_id 是 2026-11-04 随 B2 定案加的一等列（= 扫码载荷的 plan.taskId）。
        // 以前这里写 `instr(construction_info, '"construction_id":"X"') > 0`——子串匹配依赖
        // QJsonDocument::Compact 的序列化形状，键序一变、多一个空格、或将来改成 pretty 就
        // 静默查不到，而且几万行的库上它永远用不上索引。
        eq("task_id", "task_id", f.taskId);
    }

    // 围栏与类别两张表的分流：alarm_type 前缀，与 recordFromQuery 还原
    // isFenceAlarm 用的是同一条规则，两边不会漂。
    if (f.scope == AlarmFilter::ClassOnly) {
        parts << QStringLiteral("alarm_type NOT LIKE 'fence%'");
    } else if (f.scope == AlarmFilter::FenceOnly) {
        parts << QStringLiteral("alarm_type LIKE 'fence%'");
    }

    // 围栏角色：只有"离岗"是明确的 alarm_type，其余（fence_in 与老库的 fence）都算闯入禁区。
    // 用 <'' 而不是 IN ('fence','fence_in')，将来多一种围栏角色也不会在这里漏一条。
    if (f.fenceRole == AlarmFilter::RoleLeave) {
        parts << QStringLiteral("alarm_type = 'fence_out'");
    } else if (f.fenceRole == AlarmFilter::RoleIntrusion) {
        parts << QStringLiteral("alarm_type <> 'fence_out'");
    }

    out.where = QStringLiteral("WHERE ") + (parts.isEmpty() ? QStringLiteral("1=1")
                                                            : parts.join(QStringLiteral(" AND ")));
    return out;
}

void bindAll(QSqlQuery &query, const QVector<QPair<QString, QVariant>> &binds)
{
    for (const auto &b : binds) {
        query.bindValue(QLatin1Char(':') + b.first, b.second);
    }
}

} // namespace

/**
 * @brief 报警数据页的**唯一**取数入口：组合条件 + 倒序 + 分页
 * @param filter 条件结构体；语义约定全部写在 alarm_dao.h 的 AlarmFilter 上（时间毫秒、
 *               空串=不限、status 空=不含误报、channel<0=全部），这里不再重复判据
 * @return 命中行；库没开 / SQL 失败都是空表（失败多一条 qWarning，便于区分"没数据"与"查崩了"）
 *
 * WHERE 与 countByFilter 共用 buildFilterWhere，一条 SQL 两处拼是这条 API 存在的理由。
 * limit/offset 不在这里校验：SQLite 对负 limit 的处理是"不限制"，界面传错会一次拉回全库
 * ——调用方（报警查询页）必须保证 limit 是正数，默认 500。
 */
QVector<AlarmRecord> AlarmDAO::queryByFilter(const AlarmFilter &filter)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    const FilterSql fs = buildFilterWhere(filter);

    QSqlQuery query(db);
    // 排序必须带唯一列兜底：alarm_time 只到秒，同秒多条是常态（30fps 检测 + 2s 限流），
    // 只按它排则 OFFSET 翻页时同一行可能在两页都出现、或整页漏掉一行。
    query.prepare(QStringLiteral(
        "SELECT * FROM alarms %1 ORDER BY alarm_time DESC, id ASC LIMIT :__limit OFFSET :__offset")
        .arg(fs.where));
    bindAll(query, fs.binds);
    query.bindValue(":__limit", filter.limit);
    query.bindValue(":__offset", filter.offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "AlarmDAO::queryByFilter failed:" << query.lastError().text();
    }

    return results;
}

// 与 queryByFilter 必须同口径（同一个 buildFilterWhere、同一套绑定），否则分页标签上的
// "共 N 条"会和列表实际行数不一致——界面看不出差别，只有翻页时才会暴露。
// 注意这里**不**带 limit/offset：统计的是全部命中数。
int AlarmDAO::countByFilter(const AlarmFilter &filter)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    const FilterSql fs = buildFilterWhere(filter);

    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM alarms %1").arg(fs.where));
    bindAll(query, fs.binds);

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }
    qWarning() << "AlarmDAO::countByFilter failed:" << query.lastError().text();
    return 0;
}

// ----------------------------------------------------------------------------
// facets：筛选下拉的候选值 + 条数
// ----------------------------------------------------------------------------
// facet SQL 只带时间档和 scope，故意不带其它已选条件（理由见 alarm_dao.h 的声明注释）。
// 时间档为 0 时整条时间条件消失，退化成"全部历史"。
namespace {

QString andTimeRangeClause(long startTime, long endTime, QVector<QPair<QString, QVariant>> &binds)
{
    if (startTime <= 0 || endTime <= 0) return QString();
    binds.append(qMakePair(QStringLiteral("t_start"), isoOf(startTime)));
    binds.append(qMakePair(QStringLiteral("t_end"), isoOf(endTime)));
    return QStringLiteral(" AND alarm_time BETWEEN :t_start AND :t_end");
}

QString facetScopeClause(AlarmFilter::Scope scope)
{
    if (scope == AlarmFilter::ClassOnly) return QStringLiteral(" AND alarm_type NOT LIKE 'fence%'");
    if (scope == AlarmFilter::FenceOnly) return QStringLiteral(" AND alarm_type LIKE 'fence%'");
    return QString();
}

QVector<QPair<QString, int>> runFacet(const QString &sql,
                                      const QVector<QPair<QString, QVariant>> &binds,
                                      const char *who)
{
    QVector<QPair<QString, int>> out;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return out;

    QSqlQuery query(db);
    query.prepare(sql);
    bindAll(query, binds);

    if (query.exec()) {
        while (query.next()) {
            const QString value = query.value(0).toString();
            if (value.isEmpty()) continue;      // 老行没有施工信息，不配当候选项
            out.append(qMakePair(value, query.value(1).toInt()));
        }
    } else {
        qWarning() << "AlarmDAO facet" << who << "failed:" << query.lastError().text();
    }
    return out;
}

} // namespace

QVector<QPair<QString, int>> AlarmDAO::classFacets(long startTime, long endTime,
                                                   AlarmFilter::Scope scope)
{
    QVector<QPair<QString, QVariant>> binds;
    const QString sql = QStringLiteral(
        "SELECT class_name, COUNT(*) FROM alarms "
        "WHERE status <> 'false_alarm'%1%2 "
        "GROUP BY class_name ORDER BY COUNT(*) DESC LIMIT 60")
        .arg(andTimeRangeClause(startTime, endTime, binds), facetScopeClause(scope));
    return runFacet(sql, binds, "class");
}

QVector<QPair<QString, int>> AlarmDAO::workerFacets(long startTime, long endTime,
                                                    AlarmFilter::Scope scope)
{
    QVector<QPair<QString, QVariant>> binds;
    const QString sql = QStringLiteral(
        "SELECT worker_name, COUNT(*) FROM alarms "
        "WHERE status <> 'false_alarm' AND worker_name <> ''%1%2 "
        "GROUP BY worker_name ORDER BY COUNT(*) DESC LIMIT 60")
        .arg(andTimeRangeClause(startTime, endTime, binds), facetScopeClause(scope));
    return runFacet(sql, binds, "worker");
}

// 施工计划候选值：task_id 从 B2 定案起是一等列，所以这里直接 GROUP BY 它，不再把整段
// JSON 取回来在 C++ 侧解（老写法既用不上索引，也让"哪个键算 taskId"这件事有两处判据）。
// distinct 的数量＝登录过的施工计划数，通常个位数；LIMIT 60 是下拉的兜底上限。
QVector<QPair<QString, int>> AlarmDAO::taskIdFacets(long startTime, long endTime,
                                                    AlarmFilter::Scope scope)
{
    QVector<QPair<QString, QVariant>> binds;
    const QString sql = QStringLiteral(
        "SELECT task_id, COUNT(*) FROM alarms "
        "WHERE status <> 'false_alarm' AND task_id IS NOT NULL AND task_id <> ''%1%2 "
        "GROUP BY task_id ORDER BY COUNT(*) DESC LIMIT 60")
        .arg(andTimeRangeClause(startTime, endTime, binds), facetScopeClause(scope));

    QVector<QPair<QString, int>> out;
    for (const auto &row : runFacet(sql, binds, "taskId")) {
        if (!row.first.isEmpty()) out.append(qMakePair(row.first, row.second));
    }
    return out;
}

/**
 * @brief 按状态查询报警记录
 *
 * @param status: 报警状态（"pending"/"rectified"/"disposed"/"false_alarm"）
 * @param limit:  最大返回数量（默认 1000）
 * @return: 该状态的报警记录列表，按时间倒序
 */
QVector<AlarmRecord> AlarmDAO::queryByStatus(const QString &status, int limit)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE status = :status
        ORDER BY alarm_time DESC
        LIMIT :limit
    )");
    query.bindValue(":status", status);
    query.bindValue(":limit", limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

/**
 * @brief 按报警级别查询记录
 *
 * 级别定义：
 *   - 1: 紧急（urgent） - 需立即处理
 *   - 2: 重要（important） - 需尽快处理
 *   - 3: 一般（general） - 正常处理
 *   - 4: 信息（info） - 仅记录
 *
 * @param level: 报警级别（1-4）
 * @param limit: 最大返回数量（默认 1000）
 * @return: 该级别的报警记录列表，按时间倒序
 */
QVector<AlarmRecord> AlarmDAO::queryByLevel(int level, int limit)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM alarms
        WHERE alarm_level = :level
        ORDER BY alarm_time DESC
        LIMIT :limit
    )");
    query.bindValue(":level", level);
    query.bindValue(":limit", limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    }

    return results;
}

// 单行按 id 查（Pad 的 recordForId 回退路径 + 片段回填后重读都走这里）。
// "查不到"的判据是返回的 record.id 为空——AlarmRecord 没有 valid 标志，调用方别去判
// 其它字段（未登录时 workerName/constructionJson 本来就可能是空串）。
AlarmRecord AlarmDAO::queryById(const QString &id)
{
    AlarmRecord record;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return record;

    QSqlQuery query(db);
    query.prepare("SELECT * FROM alarms WHERE id = :id");
    query.bindValue(":id", id);

    if (query.exec() && query.next()) {
        record = recordFromQuery(query);
    }

    return record;
}

QVector<AlarmRecord> AlarmDAO::queryUpdatedSince(const QString &sinceInternal, int limit,
                                                 const QString &taskId)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    // 生效更新时间在 WHERE 和 ORDER BY 里各写一遍：SQLite 不认列别名用于这两个子句。
    // ⚠ 空游标必须走**不建那半条 WHERE 的路**：Qt 把空 QString 绑成 NULL，
    //   `表达式 >= NULL` 恒不成立，绑成 "" 反而一条都不回（内存库实测），
    //   症状是"首次连接的全量同步永远返回空"。
    // ⚠ 计划过滤也在这条 SQL 里做完再 LIMIT（报告 §八 P1-2）：反过来"先 LIMIT 再在 C++ 里筛"
    //   在几万行历史上会让当前计划一条都进不了这一批——游标推不动、Pad 永远同步不到、全程零报错。
    QStringList parts;
    if (!sinceInternal.isEmpty()) {
        parts << QStringLiteral("COALESCE(NULLIF(update_time, ''), alarm_time) >= :since");
    }
    if (!taskId.isEmpty()) {
        // task_id 等值（B2 定案后的一等列，判据与 buildFilterWhere 逐字同一套）。
        // 走 OR 空游标同一条教训：绑空串会让 `task_id = ''` 命中"未登录产生的那些行"，
        // 而调用方给空 taskId 的意思本来就是"不加这条条件"。
        parts << QStringLiteral("task_id = :task_id");
    }

    QSqlQuery query(db);
    QString sql = QStringLiteral("SELECT * FROM alarms");
    if (!parts.isEmpty()) {
        sql += QStringLiteral(" WHERE ") + parts.join(QStringLiteral(" AND "));
    }
    sql += QStringLiteral(
        " ORDER BY COALESCE(NULLIF(update_time, ''), alarm_time) ASC, id ASC LIMIT :limit");
    query.prepare(sql);
    if (!sinceInternal.isEmpty()) {
        query.bindValue(QStringLiteral(":since"), sinceInternal);
    }
    if (!taskId.isEmpty()) {
        query.bindValue(QStringLiteral(":task_id"), taskId);
    }
    query.bindValue(QStringLiteral(":limit"), limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query alarms updated since" << sinceInternal
                   << "taskId" << taskId
                   << "failed:" << query.lastError().text();
    }

    return results;
}

// ============================================================================
// 更新操作
// ============================================================================
// 所有更新操作遵循相同模式：
//   1. 获取数据库连接
//   2. 准备 UPDATE SQL，绑定参数
//   3. 执行并检查 numRowsAffected() 判断是否真的更新了数据
//   4. 同时更新 update_time 字段（审计追踪）
// ============================================================================

// 补充（口径已变，别只读上面那条）：update_time 不是单纯的审计字段，它是 Pad 断线补漏的
// **唯一游标**——queryUpdatedSince 按 COALESCE(NULLIF(update_time,''), alarm_time) 取数排序。
// 任何一条 UPDATE 漏写 update_time，等于这次变更对对端永久不可见（不是"晚一点同步"）。
// 另外这些函数的 false 有双重含义：SQL 失败 / 该行不存在（numRowsAffected==0），
// 二者在这里分不出来；调用方（AlarmManager）目前也不区分，只记一条日志。

bool AlarmDAO::updateStatus(const QString &id, const QString &status)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET status = :status, update_time = :update_time
        WHERE id = :id
    )");
    query.bindValue(":status", status);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Update status failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

// ============================================================================
// acknowledgeAllPending: 一条 UPDATE 把所有未确认置为已确认
// ============================================================================
// 「全部确认」这个按钮的语义就是全部，所以它必须落在库上而不是落在内存那 1000 行窗口上。
// 例外是 `disposed`：那是"已经处置过"的终态，被一句 UPDATE 收进已确认就等于本机自己
// 把 v1 §3.7「终态不能回退成 PENDING」打穿了，而且 PAD 那边会收到一帧状态倒退的 alarm_update。
int AlarmDAO::acknowledgeAllPending()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET status = 'rectified', update_time = :update_time
        WHERE status NOT IN ('rectified', 'disposed')
    )");
    query.bindValue(":update_time", currentDateTimeStr());

    if (!query.exec()) {
        qWarning() << "AlarmDAO::acknowledgeAllPending failed:" << query.lastError().text();
        return 0;
    }

    return query.numRowsAffected();
}

/**
 * @brief v1 §4.2 的处置落库：状态 + 处置记录一次写完
 *
 * 状态写 `disposed` 而不是 `rectified`：后者是本机「确认＝我看过了」，混用就等于把
 * "看过"上报成"处理过"（口径确认单 N2）。
 *
 * 三条 SQL 层的判据，都是"少写一条就会在板端出现诡异现象"的那种：
 *   1) `WHERE ... AND status NOT IN ('disposed','false_alarm')` —— 终态不回退（§3.7），
 *      也让 §4.2 的 409/2003 不需要调用方"先查状态再写"（那是个读-改-写竞争窗口）。
 *      受影响 0 行时调用方分不清"没这条"和"已终态"，用 queryById 补一次判读即可。
 *   2) `COALESCE(:x, x)` 用在 user_id/user_name/remark 上 —— 空 QString 被 Qt 绑成 NULL
 *      （这条实测过很多次，见 insertAlarms 上方那段注释），COALESCE 于是等价于
 *      "这一列这次没给，就别动"。remark 因此**不能**再由处置接口清空，
 *      那是 updateRemark() 的活：两条路各管一件事，比"一个接口既能填又能清"好排查。
 *   3) dispose_person_id 存原文、dispose_user_id 只在 personId 整体是整数时才推。
 *      把 "05-0101" 折成 5 会比不折更坏：那是一句看起来像真工号的假数（N5 的落地选择）。
 */
bool AlarmDAO::dispose(const QString &id, const QString &measure, const QString &personId,
                       const QString &personName, const QString &remark,
                       const QString &photoPathsJson, const QString &disposedAt)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    const QString now = currentDateTimeStr();
    // 对端报的处置时间优先（Pad 上可能先填表后提交），没报就用本机此刻。
    // 注意这里**不是** min(两者)：disposedAt 是"人做完处置"的时刻，
    // update_time 才是"本机改库"的时刻，两个各有其位，混成一个就没法解释重试了。
    const QString stamp = disposedAt.isEmpty() ? now : disposedAt;

    bool numericId = false;
    const int userId = personId.toInt(&numericId);

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET
            status = 'disposed',
            dispose_result = :result,
            dispose_person_id = :person_id,
            dispose_user_id = COALESCE(:user_id, dispose_user_id),
            dispose_user_name = COALESCE(:user_name, dispose_user_name),
            dispose_time = :dispose_time,
            dispose_photo = :photo,
            remark = COALESCE(:remark, remark),
            update_time = :update_time
        WHERE id = :id AND status NOT IN ('disposed', 'false_alarm')
    )");
    query.bindValue(":result", measure);
    query.bindValue(":person_id", personId);
    query.bindValue(":user_id", numericId ? QVariant(userId) : QVariant());
    query.bindValue(":user_name", personName);
    query.bindValue(":dispose_time", stamp);
    query.bindValue(":photo", photoPathsJson.isEmpty() ? QStringLiteral("[]") : photoPathsJson);
    query.bindValue(":remark", remark);
    query.bindValue(":update_time", now);
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Dispose alarm failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

/**
 * @brief 标记误报（v1 §4.3 与本机「误报」按钮共用这一条）
 *
 * 与 dispose 同一条终态判据（`status NOT IN ('disposed','false_alarm')`），
 * 于是本机界面上"对一条已处置的报警点误报"从**能改**变成**不改**。
 * 这不是回归：§3.7 明写终态不回退，而"误报覆盖处置"会让 Pad 先收到 DISPOSED
 * 再收到 FALSE_ALARM，两条都是真的、只能靠 updatedAt 猜哪个算。
 * 后四个入参全空（UI 那条路径就是这样调的）时，写进去的只有 status 与 update_time，
 * 与本次改动前逐字节相同——唯一差别是 remark 从"被空串清掉"变成"不动"（COALESCE），
 * 那个差别是修 bug：误报从来不该顺手抹掉别人写的补充说明。
 */
bool AlarmDAO::markFalseAlarm(const QString &id, const QString &remark,
                              const QString &personId, const QString &personName,
                              const QString &markedAt)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET
            status = 'false_alarm',
            remark = COALESCE(:remark, remark),
            dispose_person_id = COALESCE(:person_id, dispose_person_id),
            dispose_user_name = COALESCE(:user_name, dispose_user_name),
            dispose_time = COALESCE(:marked_at, dispose_time),
            update_time = :update_time
        WHERE id = :id AND status NOT IN ('disposed', 'false_alarm')
    )");
    query.bindValue(":remark", remark);
    query.bindValue(":person_id", personId);
    query.bindValue(":user_name", personName);
    query.bindValue(":marked_at", markedAt);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Mark false alarm failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

/**
 * @brief v1 §4.4 的已读：read_time 保留最早，update_time 每次都推
 * @param candidateTime Pad 报上来的 readAt（库内那套定宽文本）；空 = 用本机此刻
 *
 * 为什么 candidateTime 传了就**只信它**、不再与本机墙钟取小：
 *   §4.4 的语义是"只在 Pad 第一次打开详情时上报，边缘设备保留最早 readAt"。
 *   本机墙钟可能比 Pad 的校准时钟晚几秒（NTP 跳变更晚），取小就把
 *   "对端第一次打开"换成"本机收到请求"，而 §2.5 的幂等要求重试拿到同一个值——
 *   所以判据定成：**库里已有的更早就保留已有的，否则用对端报的那个**。
 * ⚠ update_time 必须每次都写：它是对端断线补漏的游标（§3.8 第 4 条），漏写等于这条已读丢了。
 */
bool AlarmDAO::markRead(const QString &id, const QString &candidateTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    const QString now = currentDateTimeStr();
    const QString candidate = candidateTime.isEmpty() ? now : candidateTime;

    QSqlQuery query(db);
    // read_time 取最早：已经读过就不动（Pad 反复上报、板端二次点开都不能把已读时间推后），
    // 但墙钟回拨时新值更早，仍以更早者为准。
    query.prepare(R"(
        UPDATE alarms SET
            read_time = CASE
                WHEN read_time IS NULL OR read_time = '' OR read_time > :candidate
                THEN :candidate
                ELSE read_time END,
            update_time = :update_time
        WHERE id = :id
    )");
    query.bindValue(":candidate", candidate);
    query.bindValue(":update_time", now);
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Mark read failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool AlarmDAO::updateRemark(const QString &id, const QString &remark)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET remark = :remark, update_time = :update_time WHERE id = :id
    )");
    query.bindValue(":remark", remark);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Update remark failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool AlarmDAO::updateImagePath(const QString &id, const QString &imagePath)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET image_path = :path, update_time = :update_time WHERE id = :id
    )");
    query.bindValue(":path", imagePath);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Update image path failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool AlarmDAO::updateVideoPath(const QString &id, const QString &videoPath)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET video_path = :path, update_time = :update_time WHERE id = :id
    )");
    query.bindValue(":path", videoPath);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Update video path failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

// ============================================================================
// 删除操作
// ============================================================================
// 注意：SQLite 不支持 TRUNCATE，使用 DELETE FROM 等效
// ============================================================================

bool AlarmDAO::remove(const QString &id)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare("DELETE FROM alarms WHERE id = :id");
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Delete alarm failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

// SQLite 没有 TRUNCATE，DELETE FROM 是全表逐行删：行数大时慢，而且**不归还磁盘空间**
// （文件只变"可复用"），所以清空之后要真空出来得另调 DatabaseManager::vacuum()。
// 与 cleanOldAlarms 同样的边界：不动媒体文件。
int AlarmDAO::clearAll()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    if (!query.exec("DELETE FROM alarms")) {
        qWarning() << "Clear alarms failed:" << query.lastError().text();
        return 0;
    }

    return query.numRowsAffected();
}

/**
 * @brief 按保留期删除历史报警
 * @param daysToKeep 保留天数，**负数会把截止时间推到未来从而删光全库**，调用方负责夹下限
 *        （AlarmManager::cleanOldData 与 ConfigManager 的 retention 配置已经夹过）
 * @return 删除行数；0 可能是"没有过期行"也可能是"库没开/SQL 失败"，判不出区别
 *
 * 两个容易踩的点：
 *   1. 比较的是 **create_time** 而不是 alarm_time：正常路径两者同刻，但补写/回灌的历史
 *      行 create_time 是入库时刻，会按"新数据"多留一个周期（可接受，反过来更危险——
 *      按 alarm_time 删会把刚回灌进来的老报警立刻删掉）；
 *   2. 本函数**不碰磁盘**：抓拍图与视频片段由 AlarmManager::pruneAlarmMedia() 按目录名
 *      日期另删一套（双闸门，见 src/biz/alarm/README.md）。两边各自失败会留下
 *      "有记录无文件"或"有文件无记录"，都不影响启动。
 */
int AlarmDAO::cleanOldAlarms(int daysToKeep)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QDateTime cutoff = QDateTime::currentDateTime().addDays(-daysToKeep);
    QString cutoffStr = cutoff.toString(Qt::ISODate);

    QSqlQuery query(db);
    query.prepare("DELETE FROM alarms WHERE create_time < :cutoff");
    query.bindValue(":cutoff", cutoffStr);

    if (!query.exec()) {
        qWarning() << "Clean old alarms failed:" << query.lastError().text();
        return 0;
    }

    return query.numRowsAffected();
}

// ============================================================================
// 统计操作
// ============================================================================
// 所有统计操作使用 SELECT COUNT(*) 或 GROUP BY 查询
// 返回 QMap 便于界面展示（key=维度，value=数量）
//
// 注意：startTime/endTime 参数预留但当前未使用（简化实现），
//       如需按时间范围统计，可扩展 WHERE 子句
// ============================================================================
// NOTE: 上面那句只对 levelStatistics()/statusStatistics() 成立（它们压根没有时间参数）。
// channelStatistics / classStatistics / typeStatistics 与 totalAlarmCount 现在都收了
// startTime/endTime 并真的下进 WHERE，别照着"预留未用"这句话去简化它们。

int AlarmDAO::totalAlarmCount(long startTime, long endTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);

    if (startTime > 0 && endTime > 0) {
        QString startStr = QDateTime::fromMSecsSinceEpoch(startTime).toString(Qt::ISODate);
        QString endStr = QDateTime::fromMSecsSinceEpoch(endTime).toString(Qt::ISODate);
        query.prepare(R"(
            SELECT COUNT(*) FROM alarms
            WHERE alarm_time BETWEEN :start AND :end
        )");
        query.bindValue(":start", startStr);
        query.bindValue(":end", endStr);
    } else {
        query.prepare("SELECT COUNT(*) FROM alarms");
    }

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

int AlarmDAO::unacknowledgedCount()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    if (query.exec("SELECT COUNT(*) FROM alarms WHERE status = 'pending'") && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

QMap<int, int> AlarmDAO::channelStatistics(long startTime, long endTime)
{
    QMap<int, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    // 时间范围必须真的进 WHERE：这三条统计原先收了 startTime/endTime 却裸 GROUP BY，
    // 于是界面无论选哪个时间档，拿到的都是全历史数字 —— 比"没有统计"更糟，因为它看起来是对的。
    QVector<QPair<QString, QVariant>> binds;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT channel, COUNT(*) as cnt FROM alarms WHERE 1=1%1 "
        "GROUP BY channel ORDER BY channel").arg(andTimeRangeClause(startTime, endTime, binds)));
    bindAll(query, binds);

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toInt()] = query.value(1).toInt();
        }
    }

    return stats;
}

QMap<QString, int> AlarmDAO::classStatistics(long startTime, long endTime)
{
    QMap<QString, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QVector<QPair<QString, QVariant>> binds;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT class_name, COUNT(*) as cnt FROM alarms WHERE 1=1%1 "
        "GROUP BY class_name ORDER BY cnt DESC").arg(andTimeRangeClause(startTime, endTime, binds)));
    bindAll(query, binds);

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toString()] = query.value(1).toInt();
        }
    }

    return stats;
}

QMap<QString, int> AlarmDAO::typeStatistics(long startTime, long endTime)
{
    QMap<QString, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QVector<QPair<QString, QVariant>> binds;
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "SELECT alarm_type, COUNT(*) as cnt FROM alarms WHERE 1=1%1 "
        "GROUP BY alarm_type ORDER BY cnt DESC").arg(andTimeRangeClause(startTime, endTime, binds)));
    bindAll(query, binds);

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toString()] = query.value(1).toInt();
        }
    }

    return stats;
}

QMap<int, int> AlarmDAO::levelStatistics()
{
    QMap<int, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT alarm_level, COUNT(*) as cnt FROM alarms
        GROUP BY alarm_level ORDER BY alarm_level
    )");

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toInt()] = query.value(1).toInt();
        }
    }

    return stats;
}

QMap<QString, int> AlarmDAO::statusStatistics()
{
    QMap<QString, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT status, COUNT(*) as cnt FROM alarms
        GROUP BY status ORDER BY cnt DESC
    )");

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toString()] = query.value(1).toInt();
        }
    }

    return stats;
}

// ============================================================================
// recordFromQuery: 从 SQL 查询结果构建 AlarmRecord 结构体
// ============================================================================
// 将 QSqlQuery 当前行的各列映射到 AlarmRecord 的成员变量：
//   - 数据库字段名（snake_case）→ C++ 成员名（camelCase）
//   - 例如：alarm_type → alarmType, class_name → className
//
// 兼容旧字段处理：
//   - timestamp: 从 alarmTime 字符串反向解析为毫秒时间戳
//   - isFenceAlarm: 由 alarmType 的 "fence" 前缀推断（fence_in/fence_out 与历史的 fence）
//
// 使用场景：
//   - 所有查询操作（queryByTimeRange, queryByChannel 等）都调用此函数
//   - 统一了 数据库行 → 内存对象 的转换逻辑
// ============================================================================
AlarmRecord AlarmDAO::recordFromQuery(QSqlQuery &query)
{
    AlarmRecord record;
    record.id = query.value("id").toString();
    record.alarmType = query.value("alarm_type").toString();
    record.alarmLevel = query.value("alarm_level").toInt();
    record.alarmTime = query.value("alarm_time").toString();
    record.channel = query.value("channel").toInt();
    record.clsId = query.value("class_id").toInt();
    record.className = query.value("class_name").toString();
    record.confidence = query.value("confidence").toFloat();
    record.imagePath = RuntimePaths::resolveStoredPath(query.value("image_path").toString());
    record.videoPath = query.value("video_path").toString();
    record.status = query.value("status").toString();
    record.disposeResult = query.value("dispose_result").toString();
    record.disposeUserId = query.value("dispose_user_id").toInt();
    record.disposeUserName = query.value("dispose_user_name").toString();
    // 老库/补列失败时这一列不存在：QSqlQuery::value(名字) 取不到就是无效值 → 空串，
    // 不会崩也不会把整条记录判废（其余各列同一条兜法）。
    record.disposePersonId = query.value("dispose_person_id").toString();
    record.disposeTime = query.value("dispose_time").toString();
    record.disposePhoto = query.value("dispose_photo").toString();
    record.remark = query.value("remark").toString();
    record.readTime = query.value("read_time").toString();
    record.createTime = query.value("create_time").toString();
    record.updateTime = query.value("update_time").toString();
    // 施工信息：老行是 NULL → 空串，显示侧成 "—"，不回填（历史上确实没有这份信息）
    record.workerName = query.value("worker_name").toString();
    record.constructionJson = query.value("construction_info").toString();
    // 施工计划身份（v1 的 taskId）：迁移前的老行这里是 NULL → 空串，出门口由 taskIdOf() 回落解 JSON
    record.taskId = query.value("task_id").toString();

    // 兼容旧字段
    // NOTE(边界): alarm_time 不是合法 ISO 文本（NULL/被外部工具改坏）时 fromString 解不出来，
    // timestamp 得到 0 —— 界面按 0 显示会成"1970-01-01"。列表用的是 alarmTime 文本所以看不出
    // 问题，只有拿 timestamp 排序/算差值的地方会踩；库内没有写入这类值的合法路径。
    record.timestamp = QDateTime::fromString(record.alarmTime, Qt::ISODate).toMSecsSinceEpoch();
    // 围栏报警有两种角色：fence_in（闯入禁区）/ fence_out（离岗），
    // 老库里存的是不带后缀的 "fence"。前缀判断把三代都认下来 —— 漏掉这条会让
    // 重启后回读的围栏报警掉进"类别"Tab，显示成 "person 检测到报警"。
    record.isFenceAlarm = record.alarmType.startsWith(QLatin1String("fence"));

    return record;
}
