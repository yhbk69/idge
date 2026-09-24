#ifndef ROLL_CALL_WIDGET_H
#define ROLL_CALL_WIDGET_H

/**
 * @file roll_call_widget.h
 * @brief 人员点名（签到）任务列表页 —— caichao 分支合入的业务页面
 *
 * 职责：嵌入 frmMain 的 pageRoll，展示点名任务列表，
 *       并提供 创建/查看/注销/删除/导出报告 等入口。
 *       具体的拍照（PhotoSelectionDialog）、识别结果（RecognitionResultDialog）、
 *       注销匹配（CancellationResultDialog）均由本页面按需以模态对话框拉起。
 */

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <memory>
#include "roll_call_service.h"

/**
 * @class RollCallWidget
 * @brief 点名任务管理页。
 *
 * 设计要点：
 *   - 服务注入式构造：构造时只做 setupUI()，RollCallService 由 frmMain
 *     在服务初始化成功后通过 setService() 注入（服务所有权在 frmMain，
 *     这里持 shared_ptr 共享）。service_ 为空时所有数据库访问均早退，
 *     页面降级为空白列表而不崩溃。
 *   - 表格刷新（loadTasks）在 GUI 线程同步读 SQLite，任务量小可接受；
 *     每行"操作"列是 setCellWidget 塞入的临时 QWidget，刷新时整表重建，
 *     lambda 按值捕获 task_id，避免闭包持有已失效的行号。
 */
class RollCallWidget : public QWidget {
    Q_OBJECT

public:
    explicit RollCallWidget(QWidget* parent = nullptr);
    ~RollCallWidget();
    
    /**
     * @brief 注入点名服务并立即刷新任务列表
     * @param service frmMain 初始化的 RollCallService（shared_ptr 共享所有权）
     */
    void setService(std::shared_ptr<RollCallService> service);
    void refreshTaskList();   ///< 重新从数据库加载任务列表（创建/删除/注销确认后调用）

private slots:
    void onCreateTask();            ///< 创建登记任务：输入名称→选照片→识别→确认，中途取消会回滚 deleteTask
    void onViewTask(int task_id);   ///< 打开任务详情（非模态顶层窗口）
    void onDeleteTask(int task_id); ///< 二次确认后删除任务及其数据/文件
    void onCancelTask(int task_id); ///< 注销流程：重新选照片→匹配已登记人脸→确认注销
    void onRefresh();               ///< 手动刷新按钮

private:
    void setupUI();
    void loadTasks();
    void showTaskDetailDialog(int task_id);
    void exportTaskReport(int task_id); ///< 导出纯文本 txt 报告（UTF-8）
    
    std::shared_ptr<RollCallService> service_;  ///< 可为空（服务初始化失败时），使用前必须判空
    
    // UI组件
    QTableWidget* task_table_;     ///< 任务列表（5列：名称/时间/人数/状态/操作）
    QPushButton* create_btn_;
    QPushButton* refresh_btn_;
};

#endif // ROLL_CALL_WIDGET_H
