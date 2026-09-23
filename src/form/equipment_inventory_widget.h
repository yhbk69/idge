#ifndef EQUIPMENT_INVENTORY_WIDGET_H
#define EQUIPMENT_INVENTORY_WIDGET_H

#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <memory>

#include "../service/equipment_inventory_service.h"

class EquipmentInventoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit EquipmentInventoryWidget(QWidget* parent = nullptr);
    void setServices(std::shared_ptr<EquipmentInventoryService> equipment_service,
                     std::shared_ptr<RollCallService> roll_call_service);
    void refreshTaskList();

private slots:
    void onCreateTask();
    void onViewTask(int task_id);
    void onCancelTask(int task_id);
    void onDeleteTask(int task_id);

private:
    void setupUi();
    void loadTasks();

    std::shared_ptr<EquipmentInventoryService> equipment_service_;
    std::shared_ptr<RollCallService> roll_call_service_;
    QTableWidget* task_table_ = nullptr;
    QPushButton* create_button_ = nullptr;
    QPushButton* refresh_button_ = nullptr;
};

#endif
