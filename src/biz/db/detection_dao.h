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
//   // 查询（时间/通道/类别都是可选条件，空=全部）
//   DetectionFilter f;
//   f.startTime = QDateTime::currentMSecsSinceEpoch() - 3600 * 1000;
//   f.endTime = QDateTime::currentMSecsSinceEpoch();
//   f.channel = 0;              // -1 = 全部通道
//   auto records = dao.queryByFilter(f);
//   int total = dao.countByFilter(f);   // 同口径总数，分页标签用
//
// ============================================================================

#include <QObject>
#include <QVector>
#include <QString>
#include <QPair>
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

// ============================================================================
// DetectionFilter - 检测明细检索条件
// ============================================================================
// 与 AlarmFilter 同形但**故意不复用同一个类型**：detections 表没有状态、没有施工信息、
// 也不分围栏/类别，套 AlarmFilter 会让一半字段在这里静默失效——那正是报警列表过去的病根
// （条件写了没人调，界面退化成读内存队列）。
// 时间一律毫秒；本表 timestamp 是 INTEGER 毫秒（写入口 insertFromDetectResult 已从纳秒换算），
// 所以这里直接比整数，不吃报警表那套"定宽 ISO 文本字典序"的前提。
struct DetectionFilter {
    long startTime = 0;     // 0 = 不限
    long endTime = 0;       // 0 = 不限
    int channel = -1;       // < 0 = 全部通道
    QString className;      // 空 = 全部类别
    int limit = 500;
    int offset = 0;
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

    // 组合条件检索（检测明细页唯一入口，倒序 + 分页）。
    // 下面这四个函数原先是「一个条件一个函数」：queryByTimeRange / queryByChannel /
    // queryByClass / queryByChannelAndClass —— 四个近重复实现、时间通道类别都必传（没有"全部"），
    // 而且全仓零调用。合并成一个带可选条件的 filter 之后，"全部通道 + 某个类别 + 今天"
    // 这种真实要问的组合才有地方问。
    QVector<DetectionRecord> queryByFilter(const DetectionFilter &filter);

    // 同口径总数，分页标签用
    int countByFilter(const DetectionFilter &filter);

    // 类别候选值 + 该时间档内的条数（筛选下拉用；只按时间收敛，与 AlarmDAO 的 facets 同一口径）
    QVector<QPair<QString, int>> classFacets(long startTime, long endTime);

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
