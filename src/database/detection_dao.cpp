// ============================================================================
// detection_dao.cpp - 识别信息数据访问对象实现
// ============================================================================
//
// 作用：
//   提供检测结果（识别信息）的数据库CRUD操作。
//
// ============================================================================

#include "detection_dao.h"
#include "database_manager.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QVariant>
#include <QDebug>

DetectionDAO::DetectionDAO(QObject *parent)
    : QObject(parent)
{
}

// ============================================================================
// insertDetection: 插入单条检测记录
// ============================================================================
int64_t DetectionDAO::insertDetection(const DetectionRecord &record)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        return -1;
    }

    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO detections (timestamp, channel, frame_id, class_id, class_name,
                               confidence, bbox_left, bbox_top, bbox_right, bbox_bottom)
        VALUES (:timestamp, :channel, :frame_id, :class_id, :class_name,
                :confidence, :bbox_left, :bbox_top, :bbox_right, :bbox_bottom)
    )");

    query.bindValue(":timestamp", static_cast<qlonglong>(record.timestamp));
    query.bindValue(":channel", record.channel);
    query.bindValue(":frame_id", static_cast<qlonglong>(record.frameId));
    query.bindValue(":class_id", record.classId);
    query.bindValue(":class_name", record.className);
    query.bindValue(":confidence", static_cast<double>(record.confidence));
    query.bindValue(":bbox_left", record.bboxLeft);
    query.bindValue(":bbox_top", record.bboxTop);
    query.bindValue(":bbox_right", record.bboxRight);
    query.bindValue(":bbox_bottom", record.bboxBottom);

    if (!query.exec()) {
        qWarning() << "Insert detection failed:" << query.lastError().text();
        return -1;
    }

    return query.lastInsertId().toLongLong();
}

// ============================================================================
// insertDetections: 批量插入检测记录
// ============================================================================
int64_t DetectionDAO::insertDetections(const QVector<DetectionRecord> &records)
{
    if (records.isEmpty()) {
        return 0;
    }

    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) {
        return 0;
    }

    int64_t inserted = 0;

    // 使用事务批量插入
    db.transaction();

    QSqlQuery query(db);
    query.prepare(R"(
        INSERT INTO detections (timestamp, channel, frame_id, class_id, class_name,
                               confidence, bbox_left, bbox_top, bbox_right, bbox_bottom)
        VALUES (:timestamp, :channel, :frame_id, :class_id, :class_name,
                :confidence, :bbox_left, :bbox_top, :bbox_right, :bbox_bottom)
    )");

    for (const DetectionRecord &record : records) {
        query.bindValue(":timestamp", static_cast<qlonglong>(record.timestamp));
        query.bindValue(":channel", record.channel);
        query.bindValue(":frame_id", static_cast<qlonglong>(record.frameId));
        query.bindValue(":class_id", record.classId);
        query.bindValue(":class_name", record.className);
        query.bindValue(":confidence", static_cast<double>(record.confidence));
        query.bindValue(":bbox_left", record.bboxLeft);
        query.bindValue(":bbox_top", record.bboxTop);
        query.bindValue(":bbox_right", record.bboxRight);
        query.bindValue(":bbox_bottom", record.bboxBottom);

        if (query.exec()) {
            inserted++;
        } else {
            qWarning() << "Insert detection failed:" << query.lastError().text();
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
// insertFromDetectResult: 从 object_detect_result_list 插入
// ============================================================================
int64_t DetectionDAO::insertFromDetectResult(int channel,
                                             const object_detect_result_list &results,
                                             const QStringList &classNames)
{
    QVector<DetectionRecord> records;
    records.reserve(results.count);

    for (int i = 0; i < results.count; i++) {
        const object_detect_result &det = results.results[i];

        DetectionRecord record;
        record.timestamp = results.time;
        record.channel = channel;
        record.frameId = results.id;
        record.classId = det.cls_id;

        // 将 cls_id 转换为类别名称
        if (det.cls_id >= 0 && det.cls_id < classNames.size()) {
            record.className = classNames[det.cls_id];
        } else {
            record.className = QString("cls_%1").arg(det.cls_id);
        }

        record.confidence = det.prop;
        record.bboxLeft = det.box.left;
        record.bboxTop = det.box.top;
        record.bboxRight = det.box.right;
        record.bboxBottom = det.box.bottom;

        records.append(record);
    }

    return insertDetections(records);
}

// ============================================================================
// queryByTimeRange: 按时间范围查询
// ============================================================================
QVector<DetectionRecord> DetectionDAO::queryByTimeRange(long startTime, long endTime,
                                                         int limit, int offset)
{
    QVector<DetectionRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM detections
        WHERE timestamp BETWEEN :start AND :end
        ORDER BY timestamp DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query by time range failed:" << query.lastError().text();
    }

    return results;
}

// ============================================================================
// queryByChannel: 按通道查询
// ============================================================================
QVector<DetectionRecord> DetectionDAO::queryByChannel(int channel,
                                                       long startTime, long endTime,
                                                       int limit, int offset)
{
    QVector<DetectionRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM detections
        WHERE channel = :channel AND timestamp BETWEEN :start AND :end
        ORDER BY timestamp DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":channel", channel);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query by channel failed:" << query.lastError().text();
    }

    return results;
}

// ============================================================================
// queryByClass: 按类别查询
// ============================================================================
QVector<DetectionRecord> DetectionDAO::queryByClass(const QString &className,
                                                    long startTime, long endTime,
                                                    int limit, int offset)
{
    QVector<DetectionRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM detections
        WHERE class_name = :class_name AND timestamp BETWEEN :start AND :end
        ORDER BY timestamp DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":class_name", className);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query by class failed:" << query.lastError().text();
    }

    return results;
}

// ============================================================================
// queryByChannelAndClass: 按通道+类别查询
// ============================================================================
QVector<DetectionRecord> DetectionDAO::queryByChannelAndClass(int channel,
                                                               const QString &className,
                                                               long startTime, long endTime,
                                                               int limit, int offset)
{
    QVector<DetectionRecord> results;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return results;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT * FROM detections
        WHERE channel = :channel AND class_name = :class_name
              AND timestamp BETWEEN :start AND :end
        ORDER BY timestamp DESC
        LIMIT :limit OFFSET :offset
    )");
    query.bindValue(":channel", channel);
    query.bindValue(":class_name", className);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));
    query.bindValue(":limit", limit);
    query.bindValue(":offset", offset);

    if (query.exec()) {
        while (query.next()) {
            results.append(recordFromQuery(query));
        }
    } else {
        qWarning() << "Query by channel and class failed:" << query.lastError().text();
    }

    return results;
}

// ============================================================================
// queryById: 获取单条记录
// ============================================================================
DetectionRecord DetectionDAO::queryById(int64_t id)
{
    DetectionRecord record;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return record;

    QSqlQuery query(db);
    query.prepare("SELECT * FROM detections WHERE id = :id");
    query.bindValue(":id", static_cast<qlonglong>(id));

    if (query.exec() && query.next()) {
        record = recordFromQuery(query);
    }

    return record;
}

// ============================================================================
// countByChannel: 按通道统计
// ============================================================================
int DetectionDAO::countByChannel(int channel, long startTime, long endTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT COUNT(*) FROM detections
        WHERE channel = :channel AND timestamp BETWEEN :start AND :end
    )");
    query.bindValue(":channel", channel);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

// ============================================================================
// countByClass: 按类别统计
// ============================================================================
int DetectionDAO::countByClass(const QString &className, long startTime, long endTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT COUNT(*) FROM detections
        WHERE class_name = :class_name AND timestamp BETWEEN :start AND :end
    )");
    query.bindValue(":class_name", className);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

// ============================================================================
// countByChannelAndClass: 按通道+类别统计
// ============================================================================
int DetectionDAO::countByChannelAndClass(int channel, const QString &className,
                                         long startTime, long endTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT COUNT(*) FROM detections
        WHERE channel = :channel AND class_name = :class_name
              AND timestamp BETWEEN :start AND :end
    )");
    query.bindValue(":channel", channel);
    query.bindValue(":class_name", className);
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

// ============================================================================
// classStatistics: 获取各类别检测统计
// ============================================================================
QMap<QString, int> DetectionDAO::classStatistics(long startTime, long endTime)
{
    QMap<QString, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT class_name, COUNT(*) as cnt FROM detections
        WHERE timestamp BETWEEN :start AND :end
        GROUP BY class_name
        ORDER BY cnt DESC
    )");
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toString()] = query.value(1).toInt();
        }
    }

    return stats;
}

// ============================================================================
// channelStatistics: 获取各通道检测统计
// ============================================================================
QMap<int, int> DetectionDAO::channelStatistics(long startTime, long endTime)
{
    QMap<int, int> stats;
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return stats;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT channel, COUNT(*) as cnt FROM detections
        WHERE timestamp BETWEEN :start AND :end
        GROUP BY channel
        ORDER BY channel
    )");
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec()) {
        while (query.next()) {
            stats[query.value(0).toInt()] = query.value(1).toInt();
        }
    }

    return stats;
}

// ============================================================================
// totalCount: 获取总检测次数
// ============================================================================
int DetectionDAO::totalCount(long startTime, long endTime)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    query.prepare(R"(
        SELECT COUNT(*) FROM detections
        WHERE timestamp BETWEEN :start AND :end
    )");
    query.bindValue(":start", static_cast<qlonglong>(startTime));
    query.bindValue(":end", static_cast<qlonglong>(endTime));

    if (query.exec() && query.next()) {
        return query.value(0).toInt();
    }

    return 0;
}

// ============================================================================
// remove: 删除单条记录
// ============================================================================
bool DetectionDAO::remove(int64_t id)
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare("DELETE FROM detections WHERE id = :id");
    query.bindValue(":id", static_cast<qlonglong>(id));

    if (!query.exec()) {
        qWarning() << "Delete detection failed:" << query.lastError().text();
        return false;
    }

    return query.numRowsAffected() > 0;
}

// ============================================================================
// clearAll: 清空所有检测记录
// ============================================================================
int DetectionDAO::clearAll()
{
    QSqlDatabase db = DatabaseManager::instance().database();
    if (!db.isOpen()) return 0;

    QSqlQuery query(db);
    if (!query.exec("DELETE FROM detections")) {
        qWarning() << "Clear detections failed:" << query.lastError().text();
        return 0;
    }

    int deleted = query.numRowsAffected();
    qInfo() << "Cleared" << deleted << "detection records";
    return deleted;
}

// ============================================================================
// recordFromQuery: 从查询结果构建 DetectionRecord
// ============================================================================
DetectionRecord DetectionDAO::recordFromQuery(QSqlQuery &query)
{
    DetectionRecord record;
    record.id = query.value("id").toLongLong();
    record.timestamp = query.value("timestamp").toLongLong();
    record.channel = query.value("channel").toInt();
    record.frameId = query.value("frame_id").toLongLong();
    record.classId = query.value("class_id").toInt();
    record.className = query.value("class_name").toString();
    record.confidence = query.value("confidence").toFloat();
    record.bboxLeft = query.value("bbox_left").toInt();
    record.bboxTop = query.value("bbox_top").toInt();
    record.bboxRight = query.value("bbox_right").toInt();
    record.bboxBottom = query.value("bbox_bottom").toInt();
    return record;
}
