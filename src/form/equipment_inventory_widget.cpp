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
    setStyleSheet(QStringLiteral("QWidget { background:#05070C; color:#E5E7EB; }"));

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("设备盘点"), this);
    QFont title_font;
    title_font.setPointSize(24);
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
        "QPushButton { background:#1E2636; color:#E5E7EB; border:none; border-radius:12px; "
        "padding:8px 24px; font-size:15px; font-weight:600; }"
        "QPushButton:hover { background:#161D2B; } QPushButton:pressed { background:#0F131C; }"));
    create_button_->setMinimumSize(140, 48);
    create_button_->setStyleSheet(QStringLiteral(
        "QPushButton { background:#38BDF8; color:#05070C; border:none; border-radius:12px; "
        "padding:8px 32px; font-size:16px; font-weight:700; }"
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
        "QTableWidget { background:#0A0D12; border:2px solid #161D2B; border-radius:12px; "
        "gridline-color:#161D2B; color:#E5E7EB; font-size:14px; }"
        "QTableWidget::item { padding:16px 20px; border-bottom:1px solid #0F131C; }"
        "QTableWidget::item:selected { background:#161D2B; }"
        "QHeaderView::section { background:#0F131C; color:#9CA3AF; padding:16px 20px; "
        "border:none; border-bottom:2px solid #38BDF8; font-size:14px; font-weight:600; }"));
    root->addWidget(task_table_, 1);
}

void EquipmentInventoryWidget::refreshTaskList() { loadTasks(); }

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
        count->setForeground(QColor("#6EE7B7")); count->setTextAlignment(Qt::AlignCenter);
        count->setFont(QFont("", 16, QFont::Bold)); task_table_->setItem(row, 2, count);
        auto* status = new QTableWidgetItem(task.is_cancelled ? QStringLiteral("已注销") : QStringLiteral("处理中"));
        status->setForeground(task.is_cancelled ? QColor("#6EE7B7") : QColor("#E9A568"));
        status->setTextAlignment(Qt::AlignCenter); status->setFont(QFont("", 13, QFont::Bold));
        task_table_->setItem(row, 3, status);

        auto* actions = new QWidget(task_table_);
        auto* layout = new QHBoxLayout(actions);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(10);
        const QString button_style = QStringLiteral(
            "QPushButton { background:#1E2636; color:%1; border:none; border-radius:8px; "
            "padding:6px 16px; font-size:13px; font-weight:600; }"
            "QPushButton:hover { background:%1; color:white; } "
            "QPushButton:pressed { background:#0F131C; color:white; }");
        auto* view = new QPushButton(QStringLiteral("查看"), actions); view->setFixedSize(82, 38); view->setStyleSheet(button_style.arg("#38BDF8"));
        auto* delete_button = new QPushButton(QStringLiteral("删除"), actions); delete_button->setFixedSize(82, 38); delete_button->setStyleSheet(button_style.arg("#DC2626"));
        layout->addWidget(view);
        if (!task.is_cancelled) {
            auto* cancel = new QPushButton(QStringLiteral("注销"), actions); cancel->setFixedSize(82, 38); cancel->setStyleSheet(button_style.arg("#E9A568"));
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
        "QInputDialog QLabel { font-size:28px; }"
        "QInputDialog QLineEdit { min-width:720px; min-height:84px; padding:12px 20px; font-size:28px; }"
        "QInputDialog QPushButton { min-width:152px; min-height:68px; font-size:28px; }"));
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

void EquipmentInventoryWidget::onViewTask(int task_id) {
    EquipmentDetailDialog detail(task_id, equipment_service_, this);
    detail.exec();
}

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

void EquipmentInventoryWidget::onDeleteTask(int task_id) {
    const Task task = equipment_service_->getTaskInfo(task_id);
    if (QMessageBox::question(this, QStringLiteral("确认删除"),
                              QStringLiteral("确定删除任务“%1”吗？").arg(QString::fromUtf8(task.name.c_str())))
        != QMessageBox::Yes) return;
    if (!equipment_service_->deleteTask(task_id))
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("删除任务失败"));
    refreshTaskList();
}
