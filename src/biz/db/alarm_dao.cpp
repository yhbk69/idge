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

AlarmDAO::AlarmDAO(QObject *parent)
    : QObject(parent)
{
}

// ============================================================================
// 生成 UUID
// ============================================================================
static QString generateUuid()
{
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// ============================================================================
// 获取当前时间字符串 (ISO8601格式)
// ============================================================================
static QString currentDateTimeStr()
{
    return QDateTime::currentDateTime().toString(Qt::ISODate);
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
                           create_time, update_time, worker_name, construction_info)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time, :worker_name, :construction_info)
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
                           create_time, update_time, worker_name, construction_info)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time, :worker_name, :construction_info)
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

    if (!f.constructionId.isEmpty()) {
        // 用 instr 而不是 json_extract：本机构建用的 Qt QSQLITE 插件内嵌的是
        // SQLite 3.31.1 且**没有编入 JSON1**（插件里搜不到任何 json_* 函数名），
        // json_extract 会直接 "no such function" 把整条查询打死。
        // 也不用 LIKE：LIKE 的 needle 里 % 和 _ 是通配符，施工id 带下划线就会过度匹配
        // （本项目的键名恰恰是 construction_id 这种下划线风格）。
        // needle 是整段 "..." 带引号一起 bind 的，序列化形状由 ConstructionInfo::toJson
        // 固定（QJsonDocument::Compact，无空格），所以这个子串匹配是精确的。
        parts << QStringLiteral("instr(construction_info, :cid_needle) > 0");
        out.binds.append(qMakePair(QStringLiteral("cid_needle"),
                                   QStringLiteral("\"construction_id\":\"%1\"")
                                       .arg(f.constructionId)));
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

// 施工id 藏在 construction_info 这段 JSON 文本里，SQLite 又没有 JSON1，
// 所以候选值不在 SQL 里解，而是把 distinct 载荷取回来在 C++ 侧用 QJsonDocument 解。
// distinct 的载荷数量 = 登录次数（同一时段所有报警共用同一份文本），是个个位数的量。
QVector<QPair<QString, int>> AlarmDAO::constructionFacets(long startTime, long endTime,
                                                          AlarmFilter::Scope scope)
{
    QVector<QPair<QString, QVariant>> binds;
    const QString sql = QStringLiteral(
        "SELECT construction_info, COUNT(*) FROM alarms "
        "WHERE status <> 'false_alarm' AND construction_info <> ''%1%2 "
        "GROUP BY construction_info ORDER BY COUNT(*) DESC LIMIT 60")
        .arg(andTimeRangeClause(startTime, endTime, binds), facetScopeClause(scope));

    QVector<QPair<QString, int>> out;
    for (const auto &row : runFacet(sql, binds, "construction")) {
        const QJsonDocument doc = QJsonDocument::fromJson(row.first.toUtf8());
        if (!doc.isObject()) continue;
        const QString id = doc.object()
                               .value(QLatin1String(session::ConstructionInfo::kKeyConstructionId))
                               .toString();
        if (!id.isEmpty()) out.append(qMakePair(id, row.second));
    }
    return out;
}

/**
 * @brief 按状态查询报警记录
 *
 * @param status: 报警状态（"pending"/"rectified"/"false_alarm"）
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

QVector<AlarmRecord> AlarmDAO::queryUpdatedSince(const QString &sinceInternal, int limit)
{
    QVector<AlarmRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    // 生效更新时间在 WHERE 和 ORDER BY 里各写一遍：SQLite 不认列别名用于这两个子句。
    // ⚠ 空游标必须走**没有 WHERE 的那条**：Qt 把空 QString 绑成 NULL，
    //   `表达式 >= NULL` 恒不成立，绑 "" 反而一条都不回（内存库实测），
    //   症状是"首次连接的全量同步永远返回空"。
    QSqlQuery query(db);
    const QString order = QStringLiteral(
        " ORDER BY COALESCE(NULLIF(update_time, ''), alarm_time) ASC, id ASC LIMIT :limit");
    if (sinceInternal.isEmpty()) {
        query.prepare(QStringLiteral("SELECT * FROM alarms") + order);
    } else {
        query.prepare(QStringLiteral(
            "SELECT * FROM alarms WHERE COALESCE(NULLIF(update_time, ''), alarm_time) >= :since")
            + order);
        query.bindValue(":since", sinceInternal);
    }
    query.bindValue(":limit", limit);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query alarms updated since" << sinceInternal
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
int AlarmDAO::acknowledgeAllPending()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET status = 'rectified', update_time = :update_time
        WHERE status <> 'rectified'
    )");
    query.bindValue(":update_time", currentDateTimeStr());

    if (!query.exec()) {
        qWarning() << "AlarmDAO::acknowledgeAllPending failed:" << query.lastError().text();
        return 0;
    }

    return query.numRowsAffected();
}

bool AlarmDAO::dispose(const QString &id, const QString &result,
                       int userId, const QString &userName,
                       const QString &remark)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET
            status = 'rectified',
            dispose_result = :result,
            dispose_user_id = :user_id,
            dispose_user_name = :user_name,
            dispose_time = :dispose_time,
            remark = :remark,
            update_time = :update_time
        WHERE id = :id
    )");
    query.bindValue(":result", result);
    query.bindValue(":user_id", userId);
    query.bindValue(":user_name", userName);
    query.bindValue(":dispose_time", currentDateTimeStr());
    query.bindValue(":remark", remark);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Dispose alarm failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool AlarmDAO::markFalseAlarm(const QString &id, const QString &remark)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET
            status = 'false_alarm',
            remark = :remark,
            update_time = :update_time
        WHERE id = :id
    )");
    query.bindValue(":remark", remark);
    query.bindValue(":update_time", currentDateTimeStr());
    query.bindValue(":id", id);

    if (!query.exec()) {
        qWarning() << "Mark false alarm failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

bool AlarmDAO::markRead(const QString &id)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    const QString now = currentDateTimeStr();
    QSqlQuery query(db);
    // read_time 取最早：已经读过就不动（Pad 反复上报、板端二次点开都不能把已读时间推后），
    // 但墙钟回拨时新值更早，仍以更早者为准。update_time 必须每次都写：
    // 它是对端断线补漏的游标（见 plan/idge-pad-v1.md §3.8 第 4 条），漏写等于这条已读丢了。
    query.prepare(R"(
        UPDATE alarms SET
            read_time = CASE
                WHEN read_time IS NULL OR read_time = '' OR read_time > :now_then
                THEN :read_time
                ELSE read_time END,
            update_time = :update_time
        WHERE id = :id
    )");
    query.bindValue(":now_then", now);
    query.bindValue(":read_time", now);
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
    record.disposeTime = query.value("dispose_time").toString();
    record.disposePhoto = query.value("dispose_photo").toString();
    record.remark = query.value("remark").toString();
    record.readTime = query.value("read_time").toString();
    record.createTime = query.value("create_time").toString();
    record.updateTime = query.value("update_time").toString();
    // 施工信息：老行是 NULL → 空串，显示侧成 "—"，不回填（历史上确实没有这份信息）
    record.workerName = query.value("worker_name").toString();
    record.constructionJson = query.value("construction_info").toString();

    // 兼容旧字段
    record.timestamp = QDateTime::fromString(record.alarmTime, Qt::ISODate).toMSecsSinceEpoch();
    // 围栏报警有两种角色：fence_in（闯入禁区）/ fence_out（离岗），
    // 老库里存的是不带后缀的 "fence"。前缀判断把三代都认下来 —— 漏掉这条会让
    // 重启后回读的围栏报警掉进"类别"Tab，显示成 "person 检测到报警"。
    record.isFenceAlarm = record.alarmType.startsWith(QLatin1String("fence"));

    return record;
}
