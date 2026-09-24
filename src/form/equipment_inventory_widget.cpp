// 文件：equipment_inventory_widget.cpp
// 职责：设备盘点任务列表页实现（caichao 分支合入），页面风格与 RollCallWidget 保持一致
#include "equipment_inventory_widget.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <utility>
#include <fstream>

#include "equipment_detail_dialog.h"
#include "equipment_recognition_dialog.h"
#include "photo_selection_widget.h"

EquipmentInventoryWidget::EquipmentInventoryWidget(QWidget* parent) : QWidget(parent) {
    setupUi();
}

// 服务注入（由 frmMain 在两个服务初始化完成后调用）：
// 注入后立即根据 isReady() 决定"创建任务"按钮可用性——识别程序/模型
// 未部署时禁用创建，避免用户进入流程后才失败；tooltip 已提示部署要求。
void EquipmentInventoryWidget::setServices(
    std::shared_ptr<EquipmentInventoryService> equipment_service,
    std::shared_ptr<RollCallService> roll_call_service) {
    equipment_service_ = std::move(equipment_service);
    roll_call_service_ = std::move(roll_call_service);
    if (create_button_)
        create_button_->setEnabled(equipment_service_ && equipment_service_->isReady());
    refreshTaskList();
}

void EquipmentInventoryWidget::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(20);
    setStyleSheet(QStringLiteral("QWidget { background:#1e1e2e; color:#E5E7EB; }"));

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("设备盘点"), this);
    QFont title_font;
    title_font.setPixelSize(22);
    title_font.setBold(true);
    title->setFont(title_font);
    title->setStyleSheet(QStringLiteral("color:#E5E7EB;"));
    header->addWidget(title);
    header->addStretch();
    refresh_button_ = new QPushButton(QStringLiteral("刷新"), this);
    create_button_ = new QPushButton(QStringLiteral("创建任务"), this);
    create_button_->setToolTip(QStringLiteral("请先部署设备识别程序、模型和标签文件"));
    refresh_button_->setMinimumSize(100, 48);
    refresh_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#45455c; color:#E5E7EB; border:none; border-radius:12px; "
        "padding:8px 24px; font-size:18px; font-weight:600; }"
        "QPushButton:hover { background:#3d3d4d; } QPushButton:pressed { background:#2d2d3d; }"));
    create_button_->setMinimumSize(140, 48);
    create_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#4fc3f7; color:#1e1e2e; border:none; border-radius:12px; "
        "padding:8px 32px; font-size:18px; font-weight:700; }"
        "QPushButton:hover { background:#0EA5E9; } QPushButton:pressed { background:#0284C7; }"));
    header->addWidget(refresh_button_);
    header->addWidget(create_button_);
    root->addLayout(header);
    connect(refresh_button_, &QPushButton::clicked, this, &EquipmentInventoryWidget::refreshTaskList);
    connect(create_button_, &QPushButton::clicked, this, &EquipmentInventoryWidget::onCreateTask);

    task_table_ = new QTableWidget(this);
    task_table_->setColumnCount(5);
    task_table_->setHorizontalHeaderLabels({QStringLiteral("任务名称"), QStringLiteral("创建时间"),
                                            QStringLiteral("登记数量"), QStringLiteral("状态"),
                                            QStringLiteral("操作")});
    task_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    task_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    task_table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    task_table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    task_table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    task_table_->setColumnWidth(1, 260);
    task_table_->setColumnWidth(2, 180);
    task_table_->setColumnWidth(3, 180);
    task_table_->setColumnWidth(4, 340);
    task_table_->verticalHeader()->setVisible(false);
    task_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    task_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    task_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    task_table_->setShowGrid(false);
    task_table_->setAlternatingRowColors(false);
    task_table_->verticalHeader()->setDefaultSectionSize(64);
    task_table_->setStyleSheet(QStringLiteral(
        "QTableWidget { background:#262636; border:2px solid #3d3d4d; border-radius:12px; "
        "gridline-color:#3d3d4d; color:#E5E7EB; font-size:16px; }"
        "QTableWidget::item { padding:16px 20px; border-bottom:1px solid #2d2d3d; }"
        "QTableWidget::item:selected { background:#3d3d4d; }"
        "QHeaderView::section { background:#2d2d3d; color:#9CA3AF; padding:16px 20px; "
        "border:none; border-bottom:2px solid #4fc3f7; font-size:16px; font-weight:600; }"));
    root->addWidget(task_table_, 1);
}

void EquipmentInventoryWidget::refreshTaskList() { loadTasks(); }

// 同步重建任务列表：GUI 线程直接查询（数据量小）；操作列每行 new 一个
// QWidget 容器塞进 setCellWidget，刷新时旧行连同按钮整体销毁，
// lambda 按值捕获 task.id（C++14 初始化捕获），不依赖行号。
void EquipmentInventoryWidget::loadTasks() {
    if (!equipment_service_ || !task_table_) return;
    task_table_->setRowCount(0);
    for (const auto& task : equipment_service_->getEquipmentTasks()) {
        const int row = task_table_->rowCount();
        task_table_->insertRow(row);
        auto* name = new QTableWidgetItem(QString::fromUtf8(task.name.c_str()));
        name->setForeground(QColor("#E5E7EB")); name->setFont(QFont("", 14, QFont::Bold));
        task_table_->setItem(row, 0, name);
        auto* time = new QTableWidgetItem(QString::fromUtf8(task.create_time.c_str()));
        time->setForeground(QColor("#9CA3AF")); task_table_->setItem(row, 1, time);
        auto* count = new QTableWidgetItem(QStringLiteral("%1 件").arg(task.registered_count));
        count->setForeground(QColor("#4caf50")); count->setTextAlignment(Qt::AlignCenter);
        count->setFont(QFont("", 16, QFont::Bold)); task_table_->setItem(row, 2, count);
        auto* status = new QTableWidgetItem(task.is_cancelled ? QStringLiteral("已注销") : QStringLiteral("处理中"));
        status->setForeground(task.is_cancelled ? QColor("#4caf50") : QColor("#ff9800"));
        status->setTextAlignment(Qt::AlignCenter); status->setFont(QFont("", 13, QFont::Bold));
        task_table_->setItem(row, 3, status);

        auto* actions = new QWidget(task_table_);
        auto* layout = new QHBoxLayout(actions);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(10);
        const QString button_style = QStringLiteral(
            "QPushButton { background:#45455c; color:%1; border:none; border-radius:8px; "
            "padding:6px 16px; font-size:16px; font-weight:600; }"
            "QPushButton:hover { background:%1; color:white; } "
            "QPushButton:pressed { background:#2d2d3d; color:white; }");
        auto* view = new QPushButton(QStringLiteral("查看"), actions); view->setFixedSize(82, 38); view->setStyleSheet(button_style.arg("#4fc3f7"));
        auto* delete_button = new QPushButton(QStringLiteral("删除"), actions); delete_button->setFixedSize(82, 38); delete_button->setStyleSheet(button_style.arg("#f44336"));
        layout->addWidget(view);
        if (!task.is_cancelled) {
            auto* cancel = new QPushButton(QStringLiteral("注销"), actions); cancel->setFixedSize(82, 38); cancel->setStyleSheet(button_style.arg("#ff9800"));
            layout->addWidget(cancel);
            connect(cancel, &QPushButton::clicked, this,
                    [this, id = task.id]() { onCancelTask(id); });
        }
        layout->addWidget(delete_button);
        connect(view, &QPushButton::clicked, this,
                [this, id = task.id]() { onViewTask(id); });
        connect(delete_button, &QPushButton::clicked, this,
                [this, id = task.id]() { onDeleteTask(id); });
        task_table_->setCellWidget(row, 4, actions);
        task_table_->setRowHeight(row, 72);
    }
}

/**
 * @brief 创建设备盘点（登记）任务：命名 → 选照片 → 后台识别 → 确认
 *
 * 与点名流程的差异：
 *   - PhotoSelectionDialog 传入设备检测器配置（detector_configs 非空），
 *     相机预览时实时框出设备而非人脸；每个模型独占一颗 NPU 核
 *     （第1个模型 RKNN_NPU_CORE_0、第2个 RKNN_NPU_CORE_1，与 RK3588
 *     三核中保留一核给实时检测流水线的预算有关）；
 *   - 盘点任务的 task_id 由 EquipmentInventoryService 分配，但选照界面
 *     仍用 roll_call_service_ 取任务目录（PhotoSelectionDialog 签名依赖它；
 *     两个服务底层共用同一张 task 表，见 createEquipmentTask 直接调用
 *     roll_call_service_->getDatabase()->createTask，故 task_id 可互用）。
 *
 * 返回码约定与 RecognitionResultDialog 相同：2 = 用户取消（需回滚删除任务）；
 * 其余非 Accepted = 询问用户是否删除，最后统一刷新列表。
 */
void EquipmentInventoryWidget::onCreateTask() {
    if (!equipment_service_ || !roll_call_service_) return;
    const QString default_name = QStringLiteral("设备盘点_%1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy年MM月dd日 hh时mm分")));
    QInputDialog input_dialog(this);
    input_dialog.setWindowTitle(QStringLiteral("创建设备盘点任务"));
    input_dialog.setLabelText(QStringLiteral("任务名称"));
    input_dialog.setInputMode(QInputDialog::TextInput);
    input_dialog.setTextValue(default_name);
    input_dialog.setStyleSheet(QStringLiteral(
        "QInputDialog QLabel { font-size:30px; }"
        "QInputDialog QLineEdit { min-width:720px; min-height:84px; padding:12px 20px; font-size:30px; }"
        "QInputDialog QPushButton { min-width:152px; min-height:68px; font-size:30px; }"));
    input_dialog.resize(900, 260);
    const bool ok = input_dialog.exec() == QDialog::Accepted;
    const QString name = input_dialog.textValue();
    if (!ok || name.trimmed().isEmpty()) return;
    const int task_id = equipment_service_->createEquipmentTask(name.toUtf8().toStdString());
    if (task_id < 0) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("创建任务失败"));
        return;
    }

    std::vector<PreviewDetectorConfig> preview_configs;
    int model_index = 0;
    for (const auto& model : equipment_service_->modelConfigs()) {
        const rknn_core_mask core_mask = model_index == 0 ? RKNN_NPU_CORE_0 : RKNN_NPU_CORE_1;
        preview_configs.push_back({PreviewDetectorConfig::Type::Equipment,
                                   model.model_path, model.labels_path, core_mask});
        ++model_index;
    }
    PhotoSelectionDialog selection(task_id, roll_call_service_, preview_configs, this);
    if (selection.exec() != QDialog::Accepted || selection.getSelectedPhotos().isEmpty()) {
        equipment_service_->deleteTask(task_id);
        return;
    }
    EquipmentRecognitionDialog recognition(
        task_id, selection.getSelectedPhotos(), equipment_service_,
        EquipmentRecognitionDialog::Registration, this);
    const int result = recognition.exec();
    if (result == QDialog::Accepted) refreshTaskList();
    else if (result == 2) {
        equipment_service_->deleteTask(task_id);
    } else if (QMessageBox::question(
                   this, QStringLiteral("保留任务"),
                   QStringLiteral("识别结果未确认，是否删除当前设备盘点任务？")) == QMessageBox::Yes) {
        equipment_service_->deleteTask(task_id);
    }
    refreshTaskList();
}

// 详情对话框为栈上对象 + exec() 模态：关闭即析构，无残留窗口；
// 数据在构造时一次性快照，不监听后台更新
void EquipmentInventoryWidget::onViewTask(int task_id) {
    EquipmentDetailDialog detail(task_id, equipment_service_, this);
    detail.exec();
}

// 注销流程：与 onCreateTask 相同的选照/识别管线，只是
// EquipmentRecognitionDialog 传 Phase::Cancellation（与登记结果做标签数量比对），
// 取消时不回滚删除任务（登记数据仍有效）
void EquipmentInventoryWidget::onCancelTask(int task_id) {
    if (!equipment_service_ || !roll_call_service_) return;
    std::vector<PreviewDetectorConfig> preview_configs;
    int model_index = 0;
    for (const auto& model : equipment_service_->modelConfigs()) {
        const rknn_core_mask core_mask = model_index == 0 ? RKNN_NPU_CORE_0 : RKNN_NPU_CORE_1;
        preview_configs.push_back({PreviewDetectorConfig::Type::Equipment,
                                   model.model_path, model.labels_path, core_mask});
        ++model_index;
    }
    PhotoSelectionDialog selection(task_id, roll_call_service_, preview_configs, this);
    if (selection.exec() != QDialog::Accepted || selection.getSelectedPhotos().isEmpty()) return;
    EquipmentRecognitionDialog recognition(
        task_id, selection.getSelectedPhotos(), equipment_service_,
        EquipmentRecognitionDialog::Cancellation, this);
    if (recognition.exec() == QDialog::Accepted) refreshTaskList();
}

// 删除前确认；deleteTask 委托 RollCallService 删除任务记录+目录，
// 无论成败都刷新列表（失败时表格保持与库一致）
void EquipmentInventoryWidget::onDeleteTask(int task_id) {
    const Task task = equipment_service_->getTaskInfo(task_id);
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
                              QStringLiteral("确定删除任务“%1”吗？").arg(QString::fromUtf8(task.name.c_str())))
        != QMessageBox::Yes) return;
    if (!equipment_service_->deleteTask(task_id))
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("删除任务失败"));
    refreshTaskList();
}
