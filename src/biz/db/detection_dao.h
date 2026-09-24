#ifndef DETECTION_DAO_H
#define DETECTION_DAO_H

// ============================================================================
// detection_dao.h - 识别信息数据访问对象
// ============================================================================
//
// 作用：
//   提供检测结果（识别信息）的数据库CRUD操作。
//   将检测结果持久化到 detections 表。
//
// 使用方式：
//   DetectionDAO dao;
//
//   // 插入检测结果
//   DetectionRecord record;
//   record.timestamp = QDateTime::currentMSecsSinceEpoch();
//   record.channel = 0;
//   record.classId = 0;
//   record.className = "person";
//   record.confidence = 0.95f;
//   record.bboxLeft = 100;
//   record.bboxTop = 100;
//   record.bboxRight = 200;
//   record.bboxBottom = 300;
//   dao.insertDetection(record);
//
//   // 查询
//   auto records = dao.queryByTimeRange(startTime, endTime);
//
// ============================================================================

#include <QObject>
#include <QVector>
#include <QString>
#include "common.hpp"

// ============================================================================
// DetectionRecord - 检测记录结构体
// ============================================================================
struct DetectionRecord {
    int64_t id = 0;             // 数据库主键
    long timestamp = 0;         // 检测时间戳（毫秒）
    int channel = 0;            // 视频通道编号
    int64_t frameId = 0;        // 帧编号
    int classId = 0;            // 类别ID
    QString className;          // 类别名称
    float confidence = 0.0f;    // 置信度
    int bboxLeft = 0;           // 检测框左上角X
    int bboxTop = 0;            // 检测框左上角Y
    int bboxRight = 0;          // 检测框右下角X
    int bboxBottom = 0;         // 检测框右下角Y
};

class DetectionDAO : public QObject
{
    Q_OBJECT
public:
    explicit DetectionDAO(QObject *parent = nullptr);

    // ========================================================================
    // 插入操作
    // ========================================================================

    // 插入单条检测记录，返回新记录的ID
    int64_t insertDetection(const DetectionRecord &record);

    // 批量插入检测记录（使用事务，高效），返回插入的行数
    int64_t insertDetections(const QVector<DetectionRecord> &records);

    // 从 object_detect_result_list 插入（直接对接检测结果）
    // channel: 视频通道
    // results: 检测结果列表
    // classNames: 类别名称列表（用于将cls_id转换为名称）
    int64_t insertFromDetectResult(int channel,
                                   const object_detect_result_list &results,
                                   const QStringList &classNames);

    // ========================================================================
    // 查询操作
    // ========================================================================

    // 按时间范围查询
    QVector<DetectionRecord> queryByTimeRange(long startTime, long endTime,
                                               int limit = 1000, int offset = 0);

    // 按通道查询
    QVector<DetectionRecord> queryByChannel(int channel,
                                            long startTime, long endTime,
                                            int limit = 1000, int offset = 0);

    // 按类别查询
    QVector<DetectionRecord> queryByClass(const QString &className,
                                          long startTime, long endTime,
                                          int limit = 1000, int offset = 0);

    // 按通道+类别查询
    QVector<DetectionRecord> queryByChannelAndClass(int channel,
                                                    const QString &className,
                                                    long startTime, long endTime,
                                                    int limit = 1000, int offset = 0);

    // 获取单条记录
    DetectionRecord queryById(int64_t id);

    // ========================================================================
    // 统计操作
    // ========================================================================

    // 按通道统计检测次数
    int countByChannel(int channel, long startTime, long endTime);

    // 按类别统计检测次数
    int countByClass(const QString &className, long startTime, long endTime);

    // 按通道+类别统计
    int countByChannelAndClass(int channel, const QString &className,
                               long startTime, long endTime);

    // 获取各类别检测统计
    QMap<QString, int> classStatistics(long startTime, long endTime);

    // 获取各通道检测统计
    QMap<int, int> channelStatistics(long startTime, long endTime);

    // 获取总检测次数
    int totalCount(long startTime, long endTime);

    // ========================================================================
    // 删除操作
    // ========================================================================

    // 删除单条记录
    bool remove(int64_t id);

    // 清空所有检测记录
    int clearAll();

private:
    // 从查询结果构建 DetectionRecord
    DetectionRecord recordFromQuery(class QSqlQuery &query);
};

#endif // DETECTION_DAO_H
