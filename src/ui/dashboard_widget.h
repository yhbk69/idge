#ifndef DASHBOARD_WIDGET_H
#define DASHBOARD_WIDGET_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
#include <QMap>
#include <QScrollArea>

/* 
====================================================
作用：仪表盘界面组件 - 显示系统运行状态和统计信息
说明：实时监控检测数量、告警数、系统资源使用情况
====================================================
*/
class DashboardWidget : public QWidget
{
    Q_OBJECT
public:
    /* 
    ====================================================
    作用：构造函数
    说明：初始化仪表盘界面，设置定时刷新机制
    参数：parent - 父窗口指针
    ====================================================
    */
    explicit DashboardWidget(QWidget *parent = nullptr);

    /* 
    ====================================================
    作用：刷新统计信息
    说明：更新所有监控指标的显示值
    ====================================================
    */
    void refreshStats();

private slots:
    /* 
    ====================================================
    作用：定时器槽函数
    说明：定期触发统计信息更新
    ====================================================
    */
    void onTimer();

private:
    /* 
    ====================================================
    作用：设置用户界面
    说明：创建所有UI组件和布局
    ====================================================
    */
    void setupUi();

    /* 
    ====================================================
    作用：创建信息卡片
    说明：创建带标题、数值和描述的统计卡片
    参数：title - 卡片标题，unit - 单位
          valueLabel - 数值标签输出参数，descLabel - 描述标签输出参数
    返回值：创建的卡片组件
    ====================================================
    */
    QWidget *createCard(const QString &title, const QString &unit,
                        QLabel **valueLabel, QLabel **descLabel);

    /* 
    ====================================================
    统计卡片标签
    说明：显示各类统计信息的数值和描述
    ====================================================
    */
    QLabel *lblDetections_ = nullptr;      // 检测数量标签
    QLabel *lblDetectionsDesc_ = nullptr;  // 检测数量描述标签
    QLabel *lblAlarms_ = nullptr;          // 告警数量标签
    QLabel *lblAlarmsDesc_ = nullptr;      // 告警数量描述标签
    QLabel *lblChannels_ = nullptr;        // 通道数量标签
    QLabel *lblChannelsDesc_ = nullptr;    // 通道数量描述标签
    QLabel *lblUptime_ = nullptr;          // 运行时间标签
    QLabel *lblUptimeDesc_ = nullptr;      // 运行时间描述标签

    /* 
    ====================================================
    系统资源监控标签
    说明：显示CPU、内存、温度、NPU使用率
    ====================================================
    */
    QLabel *lblCpu_ = nullptr;   // CPU使用率标签
    QLabel *lblMem_ = nullptr;   // 内存使用率标签
    QLabel *lblTemp_ = nullptr;  // 温度标签
    QLabel *lblNpu_ = nullptr;   // NPU使用率标签

    /* 
    ====================================================
    类别统计组件
    说明：显示检测目标类别的统计信息
    ====================================================
    */
    QWidget *classStatsWidget_ = nullptr;      // 类别统计主组件
    QVBoxLayout *classStatsLayout_ = nullptr;  // 类别统计布局
    QScrollArea *classStatsScroll_ = nullptr;  // 类别统计滚动区域

    QTimer refreshTimer_;  // 刷新定时器
    qint64 startTime_ = 0;  // 程序启动时间戳
};

#endif // DASHBOARD_WIDGET_H