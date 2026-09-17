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
#include "alarm_manager.h"

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

    // ========================================================================
    // 更新操作
    // ========================================================================

    // 更新状态
    bool updateStatus(const QString &id, const QString &status);

    // 处置报警
    bool dispose(const QString &id, const QString &result,
                 int userId, const QString &userName,
                 const QString &remark = QString());

    // 标记为误报
    bool markFalseAlarm(const QString &id, const QString &remark = QString());

    // 标记已读
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
