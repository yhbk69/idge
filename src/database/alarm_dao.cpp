// ============================================================================
// alarm_dao.cpp - 报警信息数据访问对象实现
// ============================================================================
// 参考 wvp_safety_alarm 表结构
// ============================================================================

#include "alarm_dao.h"
#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QDateTime>
#include <QUuid>
#include <QDebug>

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
                           create_time, update_time)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time)
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

    if (!query.exec()) {
        qWarning() << "Insert alarm failed:" << query.lastError().text();
        return -1;
    }

    return 1;  // SQLite不支持lastInsertId for TEXT primary key
}

// ============================================================================
// insertAlarms: 批量插入报警记录
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
        INSERT INTO alarms (id, alarm_type, alarm_level, alarm_time, channel,
                           class_id, class_name, confidence, image_path, video_path,
                           status, dispose_result, dispose_user_id, dispose_user_name,
                           dispose_time, dispose_photo, remark, read_time,
                           create_time, update_time)
        VALUES (:id, :alarm_type, :alarm_level, :alarm_time, :channel,
                :class_id, :class_name, :confidence, :image_path, :video_path,
                :status, :dispose_result, :dispose_user_id, :dispose_user_name,
                :dispose_time, :dispose_photo, :remark, :read_time,
                :create_time, :update_time)
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

// ============================================================================
// 更新操作
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

    QSqlQuery query(db);
    query.prepare(R"(
        UPDATE alarms SET read_time = :read_time WHERE id = :id
    )");
    query.bindValue(":read_time", currentDateTimeStr());
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

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT channel, COUNT(*) as cnt FROM alarms
        GROUP BY channel ORDER BY channel
    )");

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

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT class_name, COUNT(*) as cnt FROM alarms
        GROUP BY class_name ORDER BY cnt DESC
    )");

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

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT alarm_type, COUNT(*) as cnt FROM alarms
        GROUP BY alarm_type ORDER BY cnt DESC
    )");

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
// recordFromQuery: 从查询结果构建 AlarmRecord
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
    record.imagePath = query.value("image_path").toString();
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

    // 兼容旧字段
    record.timestamp = QDateTime::fromString(record.alarmTime, Qt::ISODate).toMSecsSinceEpoch();
    record.isFenceAlarm = (record.alarmType == "fence");

    return record;
}
