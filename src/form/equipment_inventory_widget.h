#ifndef EQUIPMENT_INVENTORY_WIDGET_H
#define EQUIPMENT_INVENTORY_WIDGET_H

/**
 * @file equipment_inventory_widget.h
 * @brief 设备盘点任务列表页 —— caichao 分支合入的业务页面
 *
 * 职责：嵌入 frmMain 代码创建的 equipPage_，展示设备盘点任务列表，
 *       提供 创建/查看/注销/删除 入口。拍照选图界面复用点名模块的
 *       PhotoSelectionDialog，因此需要同时注入两个服务。
 */

#include <QPushButton>
#include <QTableWidget>
#include <QWidget>
#include <memory>

#include "../service/equipment_inventory_service.h"

/**
 * @class EquipmentInventoryWidget
 * @brief 设备盘点任务管理页（结构与 RollCallWidget 平行）。
 *
 * 双服务注入的原因：
 *   - equipment_service_：盘点业务的建任务/识别/落库；
 *   - roll_call_service_：PhotoSelectionDialog 以 RollCallService 为参数构造
 *     （负责取任务目录等），仅作拍照载体使用，不参与盘点数据。
 *     两服务底层共用同一张 task 表，故盘点 task_id 可直接传给点名侧界面。
 * 任一服务为空时相关槽早退，页面降级不崩溃。
 */
class EquipmentInventoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit EquipmentInventoryWidget(QWidget* parent = nullptr);
    /**
     * @brief 注入盘点服务 + 点名服务（frmMain 初始化成功后调用），随后刷新列表
     */
    void setServices(std::shared_ptr<EquipmentInventoryService> equipment_service,
                     std::shared_ptr<RollCallService> roll_call_service);
    void refreshTaskList();   ///< 重新加载任务列表（增删改后调用）

private slots:
    void onCreateTask();            ///< 创建=登记流程：命名→选照(设备检测配置)→识别→确认
    void onViewTask(int task_id);   ///< 打开详情对话框（模态）
    void onCancelTask(int task_id); ///< 注销流程：选照→与登记检测结果匹配→确认
    void onDeleteTask(int task_id); ///< 确认后删除任务

private:
    void setupUi();
    void loadTasks();

    std::shared_ptr<EquipmentInventoryService> equipment_service_;
    std::shared_ptr<RollCallService> roll_call_service_;
    QTableWidget* task_table_ = nullptr;      ///< 5列：名称/时间/数量/状态/操作
    QPushButton* create_button_ = nullptr;    ///< 服务未就绪(isReady)时置灰
    QPushButton* refresh_button_ = nullptr;
};

#endif
