#ifndef DASHBOARD_WIDGET_H
#define DASHBOARD_WIDGET_H

// ============================================================================
// dashboard_widget.h - 数据看板页（统计卡片 + 系统资源 + 类别排行）
// ============================================================================
//
// 【所属模块】src/ui/widgets，纯展示页：没有任何写动作，只读两个源——
//   AlarmManager 单例（内部有锁，GUI 线程直接遍历）与 /proc（同步小 I/O）。
// 【刷新模型】2s QTimer 驱动 refreshStats()，全部在 GUI 线程，一次实测 60~105ms
//   （抢 AlarmManager 锁 + 读 /proc + 重建类别统计行的 QLabel 树）。
//   定时器由 showEvent/hideEvent 开关，不在槽里判 isVisible()——连 2s 一次的
//   唤醒本身也要省（详见下面 showEvent 的注释）。
// 【CPU 差分基线是函数级 static】见 .cpp 的 readCpuUsage：多实例会互相污染，
//   本仓只有 frmMain 建一个，所以成立；要复用这一页成多实例，先把基线改成成员。
// ============================================================================

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
    // 显式停表：与 hideEvent 里那句是同一件事，留给"页面还看得见但已经要收摊"的
    // 关闭流程用（frmMain::closeEvent 先 stop() 再拆别的件，避免看板一边重建、
    // 一边 AlarmManager 正在退）。重复调用无害；停了之后 showEvent 仍会把它重新起起来。
    void stop();

protected:
    /*
    ====================================================
    作用：随页面可见性开关刷新定时器
    说明：refreshStats 一次 60~105ms（抢 AlarmManager 锁遍历全表 + 读 /proc
          + 重建类别统计行），跑在 GUI 单线程上。它只在数据看板页有意义，
          但原先定时器常驻不停，切到视频监控页后仍每 2s 挤走一次 paintGL。
          用 show/hideEvent 控表而非在槽里判 isVisible()：连 2s 一次的唤醒
          本身也省掉。
    ====================================================
    */
    void showEvent(QShowEvent *event) override;
    // 只停表，不擦数据：卡片留在原位，切回来时 showEvent 立刻用旧值先填一屏，
    // 再等下一次 2s 刷新覆盖——比"切回来先空白两秒"好，也省掉一次同步重建。
    void hideEvent(QHideEvent *event) override;

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