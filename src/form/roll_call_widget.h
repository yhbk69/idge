#ifndef ROLL_CALL_WIDGET_H
#define ROLL_CALL_WIDGET_H

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <memory>
#include "../service/roll_call_service.h"

class RollCallWidget : public QWidget {
    Q_OBJECT

public:
    explicit RollCallWidget(QWidget* parent = nullptr);
    ~RollCallWidget();
    
    void setService(std::shared_ptr<RollCallService> service);
    void refreshTaskList();

private slots:
    void onCreateTask();
    void onViewTask(int task_id);
    void onDeleteTask(int task_id);
    void onCancelTask(int task_id);
    void onRefresh();

private:
    void setupUI();
    void loadTasks();
    void showTaskDetailDialog(int task_id);
    void exportTaskReport(int task_id);
    
    std::shared_ptr<RollCallService> service_;
    
    // UI组件
    QTableWidget* task_table_;
    QPushButton* create_btn_;
    QPushButton* refresh_btn_;
};

#endif // ROLL_CALL_WIDGET_H
