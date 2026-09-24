/*
任务列表主界面（文件：roll_call_widget.cpp，实现 RollCallWidget）
展示和管理所有点名任务：
任务列表表格：显示任务名称、创建时间、签到人数、状态（处理中/已注销）
创建任务：弹出对话框输入任务名 → 选择照片 → 识别 → 确认保存
查看任务：打开详情弹窗，分栏展示：
登记/注销的后处理图片（带人脸框）
唯一人脸列表或注销匹配表
注销任务：重新拍照/上传 → 匹配已登记人脸 → 标记注销
删除任务：清除数据库和文件
导出报告：生成 txt 纯文本报告（UTF-8）
*/
#include "../utils/qt_image_utils.h"
#include "roll_call_widget.h"
#include "photo_selection_widget.h"
#include "recognition_result_dialog.h"
#include "cancellation_result_dialog.h"
#include "theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QDateTime>
#include <QPixmap>
#include <QGroupBox>
#include <QTabWidget>
#include <QScrollArea>
#include <QGridLayout>
#include <QFileDialog>
#include <QTextStream>
#include <map>
#include <utility>

RollCallWidget::RollCallWidget(QWidget* parent)
    : QWidget(parent) {
    setupUI();   // 仅构建静态 UI；服务未注入前列表保持为空
}

RollCallWidget::~RollCallWidget() = default;

// 服务注入：由 frmMain 在 RollCallService::initialize() 成功后调用。
// 注入即触发首次刷新，保证页面出现时列表与数据库一致。
void RollCallWidget::setService(std::shared_ptr<RollCallService> service) {
    service_ = service;
    refreshTaskList();
}

// 页面整体为深色卡片风格（theme::BG 背景），与 caichao 业务页统一。
// 表格列宽为经验值：时间列 260px、人数/状态列 180px、操作列 340px
// 足以容纳"查看/注销/删除"三个 82px 按钮；行高 64/72px 适配触摸操作。
void RollCallWidget::setupUI() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(20);
    
    setStyleSheet(QString("QWidget { background: %1; }").arg(theme::BG));
    
    // 顶部标题和按钮
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(16);
    
    auto* title = new QLabel("签到点名", this);
    QFont title_font;
    title_font.setPixelSize(22);
    title_font.setBold(true);
    title->setFont(title_font);
    title->setStyleSheet(QString("color: %1;").arg(theme::TEXT));
    header_layout->addWidget(title);
    
    header_layout->addStretch();
    
    refresh_btn_ = new QPushButton("刷新", this);
    refresh_btn_->setMinimumSize(100, 48);
    refresh_btn_->setStyleSheet(
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 8px 24px;"
        "  font-size:%3px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(theme::HOVER, theme::TEXT)
        .arg(theme::FS_CARD)
        .arg(theme::BORDER, theme::PANEL));
    connect(refresh_btn_, &QPushButton::clicked, this, &RollCallWidget::onRefresh);
    header_layout->addWidget(refresh_btn_);
    
    create_btn_ = new QPushButton("创建任务", this);
    create_btn_->setMinimumSize(140, 48);
    create_btn_->setStyleSheet(
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 8px 32px;"
        "  font-size:%3px;"
        "  font-weight: 700;"
        "}"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(theme::ACCENT, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SKY, theme::SKY_PRESSED));
    connect(create_btn_, &QPushButton::clicked, this, &RollCallWidget::onCreateTask);
    header_layout->addWidget(create_btn_);
    
    main_layout->addLayout(header_layout);
    
    // 任务列表表格
    task_table_ = new QTableWidget(this);
    task_table_->setColumnCount(5);
    task_table_->setHorizontalHeaderLabels({"任务名称", "创建时间", "签到人数", "状态", "操作"});
    task_table_->horizontalHeader()->setStretchLastSection(false);
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
    task_table_->setSelectionMode(QAbstractItemView::SingleSelection);
    task_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    task_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    task_table_->setShowGrid(false);
    task_table_->setAlternatingRowColors(false);
    task_table_->verticalHeader()->setDefaultSectionSize(64);
    task_table_->setStyleSheet(
        QString("QTableWidget {"
        "  background: %1;"
        "  border: 2px solid %2;"
        "  border-radius: 12px;"
        "  gridline-color: %2;"
        "  color: %3;"
        "  font-size:%4px;"
        "}"
        "QTableWidget::item {"
        "  padding: 16px 20px;"
        "  border-bottom: 1px solid %5;"
        "}"
        "QTableWidget::item:selected {"
        "  background: %2;"
        "}"
        "QHeaderView::section {"
        "  background: %5;"
        "  color: %6;"
        "  padding: 16px 20px;"
        "  border: none;"
        "  border-bottom: 2px solid %7;"
        "  font-size:%4px;"
        "  font-weight: 600;"
        "  text-align: left;"
        "}")
        .arg(theme::FIELD, theme::BORDER, theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::PANEL, theme::TEXT_MUTED, theme::ACCENT));
    main_layout->addWidget(task_table_, 1);
}

void RollCallWidget::refreshTaskList() {
    loadTasks();
}

/**
 * @brief 同步重建任务列表（GUI 线程直接查 SQLite，任务量小可接受）
 *
 * 要点：
 *   - service_ 可能为空（点名服务初始化失败），此时直接返回，页面降级为空白列表；
 *   - 只展示 type=="registration" 的登记任务（注销任务通过 is_cancelled 状态体现）；
 *   - 每行"操作"列 new 一个临时 QWidget 塞进 setCellWidget——下次刷新
 *     setRowCount(0) 会连同旧 cellWidget 一并销毁重建，因此 lambda 必须
 *     按值捕获 task_id / is_cancelled，而不是持有行号或 task 引用。
 */
void RollCallWidget::loadTasks() {
    if (!service_) return;
    
    task_table_->setRowCount(0);
    
    auto tasks = service_->getAllTasks();
    
    for (const auto& task : tasks) {
        if (task.type != "registration") continue;
        
        int row = task_table_->rowCount();
        task_table_->insertRow(row);
        task_table_->setRowHeight(row, 72);
        
        // 任务名称
        auto* name_item = new QTableWidgetItem(QString::fromStdString(task.name));
        name_item->setForeground(QBrush(QColor(theme::TEXT)));
        QFont name_font = name_item->font();
        name_font.setPixelSize(16);
        name_font.setBold(true);
        name_item->setFont(name_font);
        task_table_->setItem(row, 0, name_item);
        
        // 创建时间
        auto* time_item = new QTableWidgetItem(QString::fromStdString(task.create_time));
        time_item->setForeground(QBrush(QColor(theme::TEXT_MUTED)));
        task_table_->setItem(row, 1, time_item);
        
        // 签到人数
        auto* count_item = new QTableWidgetItem(
            QStringLiteral("%1 人").arg(task.registered_count));
        QFont count_font;
        count_font.setPixelSize(18);
        count_font.setBold(true);
        count_item->setFont(count_font);
        count_item->setForeground(QBrush(QColor(theme::SUCCESS)));
        count_item->setTextAlignment(Qt::AlignCenter);
        task_table_->setItem(row, 2, count_item);
        
        // 状态
        const QString task_status = task.is_cancelled
            ? QStringLiteral("已注销")
            : QStringLiteral("处理中");
        const QString status_color = task.is_cancelled
            ? QString(theme::SUCCESS) : QString(theme::WARNING);
        auto* status_item = new QTableWidgetItem(task_status);
        QFont status_font = status_item->font();
        status_font.setPixelSize(16);
        status_font.setBold(true);
        status_item->setFont(status_font);
        status_item->setForeground(QBrush(QColor(status_color)));
        status_item->setTextAlignment(Qt::AlignCenter);
        task_table_->setItem(row, 3, status_item);
        
        // 操作按钮
        auto* actions_widget = new QWidget();
        auto* actions_layout = new QHBoxLayout(actions_widget);
        actions_layout->setContentsMargins(12, 10, 12, 10);
        actions_layout->setSpacing(10);

        int task_id = task.id;
        bool is_cancelled = task.is_cancelled;

        const auto button_style = [](const char *accent, const char *pressed) {
            return QString(
                "QPushButton {"
                "  background: %1;"
                "  color: %2;"
                "  border: none;"
                "  border-radius: 8px;"
                "  padding: 6px 16px;"
                "  font-size:%3px;"
                "  font-weight: 600;"
                "}"
                "QPushButton:hover { background: %2; color: white; }"
                "QPushButton:pressed { background: %4; color: white; }")
                .arg(theme::HOVER, accent)
                .arg(theme::FS_BODY)
                .arg(pressed);
        };

        auto* view_btn = new QPushButton("查看", actions_widget);
        view_btn->setFixedSize(82, 38);
        view_btn->setStyleSheet(button_style(theme::ACCENT, theme::PANEL));
        connect(view_btn, &QPushButton::clicked, [this, task_id]() { onViewTask(task_id); });
        actions_layout->addWidget(view_btn);

        // 注销按钮
        if (!is_cancelled) {
            auto* cancel_btn = new QPushButton("注销", actions_widget);
            cancel_btn->setFixedSize(82, 38);
            cancel_btn->setStyleSheet(button_style(theme::WARNING, theme::PANEL));
            connect(cancel_btn, &QPushButton::clicked, [this, task_id]() { onCancelTask(task_id); });
            actions_layout->addWidget(cancel_btn);
        }

        auto* delete_btn = new QPushButton("删除", actions_widget);
        delete_btn->setFixedSize(82, 38);
        delete_btn->setStyleSheet(button_style(theme::DANGER, theme::DANGER_PRESSED));
        connect(delete_btn, &QPushButton::clicked, [this, task_id]() { onDeleteTask(task_id); });
        actions_layout->addWidget(delete_btn);

        task_table_->setCellWidget(row, 4, actions_widget);
    }
}

/**
 * @brief 创建登记任务的完整流水线（全部为模态对话框，父子链挂在本页面 this 上）
 *
 * 流程：输入任务名 → createRegistrationTask（先落库拿到 task_id）
 *       → PhotoSelectionDialog 拍照/选图 → RecognitionResultDialog NPU 识别+人工确认。
 *
 * 回滚策略（防止库里残留垃圾任务）：
 *   - 选照片被取消 / 一张照片都没有 → deleteTask 回滚；
 *   - 识别结果对话框自定义返回码 2 = 用户点"取消"且已在对话框内删除任务，
 *     外层只需提示，不再重复删除；
 *   - 其他非 Accepted 退出（关窗/上一步后放弃）→ 询问用户是否删除。
 */
void RollCallWidget::onCreateTask() {
    if (!service_) {
        QMessageBox::warning(this, "错误", "服务未初始化");
        return;
    }
    
    // 生成默认任务名
    // 大字号样式（28px/900x260）是RK3588 触摸屏可读性要求，非随意取值
    const QString default_name = QString("签到点名_%1")
        .arg(QDateTime::currentDateTime().toString("yyyy年MM月dd日 hh时mm分"));
    QInputDialog input_dialog(this);
    input_dialog.setWindowTitle("创建任务");
    input_dialog.setLabelText("任务名称");
    input_dialog.setInputMode(QInputDialog::TextInput);
    input_dialog.setTextValue(default_name);
    input_dialog.setStyleSheet(QString(
        "QInputDialog QLabel { font-size:%1px; }"
        "QInputDialog QLineEdit { min-width: 720px; min-height: 84px; padding: 12px 20px; font-size:%1px; }"
        "QInputDialog QPushButton { min-width: 152px; min-height: 68px; font-size:%1px; }")
        .arg(theme::FS_KPI));
    input_dialog.resize(900, 260);
    const bool ok = input_dialog.exec() == QDialog::Accepted;
    const QString task_name = input_dialog.textValue();
    
    if (!ok || task_name.trimmed().isEmpty()) {
        return;
    }
    
    // 创建任务
    int task_id = service_->createRegistrationTask(task_name.toStdString());
    if (task_id < 0) {
        QMessageBox::critical(this, "错误", "创建任务失败");
        return;
    }
    
    // 显示照片选择对话框
    PhotoSelectionDialog photo_dialog(task_id, service_, {}, this);
    
    if (photo_dialog.exec() == QDialog::Accepted) {
        QStringList photos = photo_dialog.getSelectedPhotos();
        
        if (photos.isEmpty()) {
            service_->deleteTask(task_id);
            return;
        }
        
        // 显示识别结果对话框
        RecognitionResultDialog result_dialog(task_id, photos, service_, this);
        
        int result = result_dialog.exec();
        
        if (result == QDialog::Accepted) {
            // 用户确认，刷新列表
            refreshTaskList();
        } else if (result == 2) {
            // 用户取消（自动关闭返回2）
            QMessageBox::information(this, "提示", "任务已取消");
        } else {
            // 询问是否保留
            auto reply = QMessageBox::question(
                this, "确认", "是否取消并删除任务？",
                QMessageBox::Yes | QMessageBox::No
            );
            
            if (reply == QMessageBox::Yes) {
                service_->deleteTask(task_id);
            }
        }
    } else {
        // 照片选择取消，删除任务
        service_->deleteTask(task_id);
    }
}

void RollCallWidget::onViewTask(int task_id) {
    showTaskDetailDialog(task_id);
}

// 删除前二次确认：deleteTask 会级联清除数据库记录与任务目录文件，不可恢复
void RollCallWidget::onDeleteTask(int task_id) {
    Task task = service_->getTaskInfo(task_id);
    if (task.id == 0) {
        QMessageBox::warning(this, "错误", "任务不存在");
        return;
    }
    
    auto reply = QMessageBox::question(
        this, "确认删除",
        QString("确定要删除任务 \"%1\" 吗？\n这将同时删除所有相关数据。\n此操作不可恢复！")
            .arg(QString::fromStdString(task.name)),
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        if (service_->deleteTask(task_id)) {
            QMessageBox::information(this, "成功", "任务已删除");
            refreshTaskList();
        } else {
            QMessageBox::critical(this, "错误", "删除任务失败");
        }
    }
}

// 注销流程：先校验任务存在且未注销 → 再次拉起 PhotoSelectionDialog 采集注销照片
// → CancellationResultDialog 后台线程匹配已登记人脸 → 确认后刷新列表
void RollCallWidget::onCancelTask(int task_id) {
    Task task = service_->getTaskInfo(task_id);
    if (task.id == 0 || task.is_cancelled) {
        QMessageBox::warning(this, "错误", "任务不存在或已经注销");
        return;
    }
    PhotoSelectionDialog photos(task_id, service_, {}, this);
    if (photos.exec() != QDialog::Accepted) return;
    CancellationResultDialog result(task_id, photos.getSelectedPhotos(), service_, this);
    if (result.exec() == QDialog::Accepted) refreshTaskList();
}

void RollCallWidget::onRefresh() {
    refreshTaskList();
}

/**
 * @brief 任务详情窗口（非模态）
 *
 * 为什么用裸 QWidget + WA_DeleteOnClose 而不是 QDialog：
 *   详情窗口需要独立顶层显示（Qt::Window），允许用户一边看详情一边操作列表；
 *   生命周期靠 Qt 对象树 + close 时自毁，不阻塞 GUI 线程，也不需要 exec()。
 *
 * 数据来源均为已落库的后处理图/人脸记录，只做展示，不触发识别：
 *   - 已注销任务：左右双栏（登记后处理图 vs 注销后处理图）+ 注销匹配表；
 *   - 未注销任务：图片双列网格 + 唯一人脸 5 列网格（120x120 头像卡）。
 * 图片统一经 loadPixmapSafe 加载（规避板端 Qt JPEG 插件与 libjpeg ABI 冲突），
 * 卡片图缩放到 600x400、匹配表头像 240x190 为触摸端可读的经验尺寸。
 */
void RollCallWidget::showTaskDetailDialog(int task_id) {
    Task task = service_->getTaskInfo(task_id);
    if (task.id == 0) {
        QMessageBox::warning(this, "错误", "任务不存在");
        return;
    }
    
    // 创建详情对话框
    auto* detail_dialog = new QWidget(nullptr);
    detail_dialog->setAttribute(Qt::WA_DeleteOnClose);
    detail_dialog->setWindowTitle(QString("任务详情 - %1").arg(QString::fromStdString(task.name)));
    detail_dialog->setWindowFlags(Qt::Window);
    detail_dialog->resize(1400, 900);
    detail_dialog->setStyleSheet(QString("QWidget { background: %1; }").arg(theme::BG));
    
    auto* main_layout = new QVBoxLayout(detail_dialog);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(20);
    
    // 任务信息
    auto* info_group = new QGroupBox("任务信息", detail_dialog);
    info_group->setStyleSheet(
        QString("QGroupBox {"
        "  background: %1;"
        "  border: 2px solid %2;"
        "  border-radius: 12px;"
        "  color: %3;"
        "  font-size:%4px;"
        "  font-weight: 600;"
        "  padding: 20px;"
        "  margin-top: 12px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  left: 16px;"
        "  padding: 0 8px;"
        "}")
        .arg(theme::FIELD, theme::BORDER, theme::TEXT)
        .arg(theme::FS_CARD));
    auto* info_layout = new QGridLayout(info_group);
    info_layout->setSpacing(16);
    info_layout->setColumnStretch(1, 1);
    
    int row = 0;
    
    auto add_info_row = [&](const QString& label, const QString& value, const QString& color = theme::TEXT) {
        auto* label_widget = new QLabel(label + ":", info_group);
        label_widget->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        info_layout->addWidget(label_widget, row, 0);
        
        auto* value_widget = new QLabel(value, info_group);
        value_widget->setStyleSheet(QString("color: %1; font-size:%2px; font-weight: 600;").arg(color).arg(theme::FS_BODY));
        info_layout->addWidget(value_widget, row, 1);
        row++;
    };
    
    add_info_row("任务名称", QString::fromStdString(task.name));
    add_info_row("创建时间", QString::fromStdString(task.create_time));
    add_info_row("登记人数", QString::number(task.registered_count), theme::SUCCESS);
    if (task.is_cancelled)
        add_info_row("注销人数", QString::number(task.cancelled_count), theme::WARNING);
    add_info_row("任务状态", task.is_cancelled ? "已注销" : "处理中",
                 task.is_cancelled ? theme::SUCCESS : theme::WARNING);
    
    main_layout->addWidget(info_group);
    
    // 选项卡
    auto* tab_widget = new QTabWidget(detail_dialog);
    tab_widget->setStyleSheet(
        QString("QTabWidget::pane {"
        "  background: %1;"
        "  border: 2px solid %2;"
        "  border-radius: 12px;"
        "  top: -2px;"
        "}"
        "QTabBar::tab {"
        "  background: %3;"
        "  color: %4;"
        "  padding: 12px 24px;"
        "  margin-right: 4px;"
        "  border-top-left-radius: 8px;"
        "  border-top-right-radius: 8px;"
        "  font-size:%5px;"
        "  font-weight: 600;"
        "}"
        "QTabBar::tab:selected {"
        "  background: %1;"
        "  color: %6;"
        "  border-bottom: 3px solid %6;"
        "}"
        "QTabBar::tab:hover {"
        "  background: %2;"
        "}")
        .arg(theme::FIELD, theme::BORDER, theme::PANEL, theme::TEXT_MUTED)
        .arg(theme::FS_BODY)
        .arg(theme::ACCENT));
    
    // 从人脸记录表按"原始照片路径"聚合出每张照片的展示项（photo_index 去重），
    // unique_count 只统计 is_duplicate==false 的人脸，与识别结果页口径一致
    TaskProcessResult registration_result;
    const auto records = service_->getDatabase()->getFaceRecordsByTask(task_id);
    std::map<std::string, size_t> photo_index;
    for (const auto& record : records) {
        size_t index = 0;
        const auto it = photo_index.find(record.original_photo_path);
        if (it == photo_index.end()) {
            index = registration_result.photos.size();
            photo_index[record.original_photo_path] = index;
            PhotoProcessResult photo;
            photo.original_path = record.original_photo_path;
            photo.processed_path = record.processed_photo_path;
            photo.unique_count = 0;
            registration_result.photos.push_back(std::move(photo));
        } else {
            index = it->second;
        }
        if (!record.is_duplicate)
            ++registration_result.photos[index].unique_count;
    }

    auto create_photo_card = [](QWidget* parent, const std::string& image_path,
                                const QString& caption) {
        auto* card = new QWidget(parent);
        card->setStyleSheet(
            QString("QWidget { background: %1; border: 2px solid %2; border-radius: 12px; }")
                .arg(theme::PANEL, theme::BORDER));
        auto* card_layout = new QVBoxLayout(card);
        card_layout->setContentsMargins(12, 12, 12, 12);
        card_layout->setSpacing(10);

        auto* image = new QLabel(card);
        image->setAlignment(Qt::AlignCenter);
        image->setStyleSheet(QString("background: %1; border: none; border-radius: 8px; padding: 8px;")
                                 .arg(theme::FIELD));
        const QPixmap pixmap = loadPixmapSafe(QString::fromStdString(image_path));
        if (!pixmap.isNull())
            image->setPixmap(pixmap.scaled(600, 400, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            image->setText(QStringLiteral("图片无法加载"));
        card_layout->addWidget(image);

        auto* text = new QLabel(caption, card);
        text->setAlignment(Qt::AlignCenter);
        text->setStyleSheet(QString("color: %1; border: none; font-size:%2px; font-weight: 700;")
                                .arg(theme::ACCENT)
                                .arg(theme::FS_CARD));
        card_layout->addWidget(text);
        return card;
    };

    // Tab 1: 处理后图片
    auto* photos_tab = new QWidget();
    auto* photos_layout = new QVBoxLayout(photos_tab);
    photos_layout->setContentsMargins(16, 16, 16, 16);
    auto* photos_scroll = new QScrollArea(photos_tab);
    photos_scroll->setWidgetResizable(true);
    photos_scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");
    auto* photos_container = new QWidget();
    photos_container->setStyleSheet("background: transparent;");

    if (task.is_cancelled) {
        auto* columns = new QHBoxLayout(photos_container);
        columns->setContentsMargins(0, 0, 0, 0);
        columns->setSpacing(16);

        auto* registration_column = new QWidget(photos_container);
        auto* registration_layout = new QVBoxLayout(registration_column);
        registration_layout->setContentsMargins(0, 0, 0, 0);
        registration_layout->setSpacing(12);
        auto* registration_title = new QLabel(QStringLiteral("登记后处理图片"), registration_column);
        registration_title->setStyleSheet(theme::text(theme::ACCENT, theme::FS_CARD, true));
        registration_layout->addWidget(registration_title);
        for (const auto& photo : registration_result.photos) {
            registration_layout->addWidget(create_photo_card(
                registration_column, photo.processed_path,
                QStringLiteral("不重复人数: %1").arg(photo.unique_count)));
        }
        registration_layout->addStretch();

        auto* cancellation_column = new QWidget(photos_container);
        auto* cancellation_layout = new QVBoxLayout(cancellation_column);
        cancellation_layout->setContentsMargins(0, 0, 0, 0);
        cancellation_layout->setSpacing(12);
        auto* cancellation_title = new QLabel(QStringLiteral("注销后处理图片"), cancellation_column);
        cancellation_title->setStyleSheet(theme::text(theme::WARNING, theme::FS_CARD, true));
        cancellation_layout->addWidget(cancellation_title);
        const auto cancellation_photos =
            service_->getDatabase()->getCancellationPhotosByTask(task_id);
        for (size_t i = 0; i < cancellation_photos.size(); ++i) {
            cancellation_layout->addWidget(create_photo_card(
                cancellation_column, cancellation_photos[i].processed_photo_path,
                QStringLiteral("注销图片 %1").arg(i + 1)));
        }
        if (cancellation_photos.empty()) {
            auto* unavailable = new QLabel(QStringLiteral("没有保存的注销后处理图片"), cancellation_column);
            unavailable->setAlignment(Qt::AlignCenter);
            unavailable->setStyleSheet(QString("color: %1; padding: 40px;").arg(theme::TEXT_MUTED));
            cancellation_layout->addWidget(unavailable);
        }
        cancellation_layout->addStretch();

        columns->addWidget(registration_column, 1);
        columns->addWidget(cancellation_column, 1);
    } else {
        auto* photos_grid = new QGridLayout(photos_container);
        photos_grid->setSpacing(16);
        int photo_row = 0;
        int photo_column = 0;
        for (const auto& photo : registration_result.photos) {
            photos_grid->addWidget(create_photo_card(
                photos_container, photo.processed_path,
                QStringLiteral("不重复人数: %1").arg(photo.unique_count)),
                photo_row, photo_column);
            if (++photo_column >= 2) {
                photo_column = 0;
                ++photo_row;
            }
        }
    }
    photos_scroll->setWidget(photos_container);
    photos_layout->addWidget(photos_scroll);
    tab_widget->addTab(photos_tab, QStringLiteral("处理后图片"));

    // Tab 2: 单一人脸
    auto* faces_tab = new QWidget();
    auto* faces_layout = new QVBoxLayout(faces_tab);
    faces_layout->setContentsMargins(16, 16, 16, 16);
    const auto unique_faces = service_->getDatabase()->getUniqueFacesByTask(task_id);

    if (task.is_cancelled) {
        const auto cancellation_matches =
            service_->getDatabase()->getCancellationMatchesByTask(task_id);
        auto* matches_table = new QTableWidget(faces_tab);
        matches_table->setColumnCount(4);
        matches_table->setHorizontalHeaderLabels({
            QStringLiteral("登记人脸"), QStringLiteral("注销人脸"),
            QStringLiteral("说明"), QStringLiteral("匹配度")});
        matches_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        matches_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        matches_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        matches_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        matches_table->verticalHeader()->setVisible(false);
        matches_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        matches_table->setSelectionMode(QAbstractItemView::NoSelection);

        auto create_face_cell = [](QWidget* parent, const std::string& path) {
            auto* image = new QLabel(parent);
            image->setFixedSize(240, 190);
            image->setAlignment(Qt::AlignCenter);
            image->setStyleSheet(
                QString("background: %1; border: 1px solid %2; border-radius: 8px;")
                    .arg(theme::FIELD, theme::HOVER));
            const QPixmap pixmap = loadPixmapSafe(QString::fromStdString(path));
            if (!pixmap.isNull())
                image->setPixmap(pixmap.scaled(image->size(), Qt::IgnoreAspectRatio,
                                                Qt::SmoothTransformation));
            else
                image->setText(QStringLiteral("-"));
            return image;
        };

        matches_table->setRowCount(static_cast<int>(cancellation_matches.size()));
        for (size_t i = 0; i < cancellation_matches.size(); ++i) {
            const auto& match = cancellation_matches[i];
            const int table_row = static_cast<int>(i);
            matches_table->setCellWidget(
                table_row, 0, create_face_cell(matches_table, match.registration_image));
            matches_table->setCellWidget(
                table_row, 1, create_face_cell(matches_table, match.cancellation_image));
            // 匹配状态码（与 RollCallService 约定一致）：
            // 1=注销匹配（有人脸相似度）, 2=未登记（注销照里的人没登记过）, 其他=未注销
            auto* description = new QTableWidgetItem(
                match.status == 1 ? QStringLiteral("注销匹配")
                : match.status == 2 ? QStringLiteral("未登记") : QStringLiteral("未注销"));
            description->setTextAlignment(Qt::AlignCenter);
            matches_table->setItem(table_row, 2, description);
            auto* similarity = new QTableWidgetItem(
                match.status == 1 ? QString::number(match.similarity, 'f', 4)
                                  : QStringLiteral("-"));
            similarity->setTextAlignment(Qt::AlignCenter);
            matches_table->setItem(table_row, 3, similarity);
            matches_table->setRowHeight(table_row, 210);
        }
        faces_layout->addWidget(matches_table);
        tab_widget->addTab(
            faces_tab, QStringLiteral("单一人脸 (%1)").arg(cancellation_matches.size()));
    } else {
        auto* faces_scroll = new QScrollArea(faces_tab);
        faces_scroll->setWidgetResizable(true);
        faces_scroll->setStyleSheet("QScrollArea { background: transparent; border: none; }");
        auto* faces_container = new QWidget();
        faces_container->setStyleSheet("background: transparent;");
        auto* faces_grid = new QGridLayout(faces_container);
        faces_grid->setSpacing(16);
        int face_row = 0;
        int face_column = 0;
        for (size_t i = 0; i < unique_faces.size(); ++i) {
            auto* face_card = new QWidget(faces_container);
            face_card->setStyleSheet(
                QString("QWidget { background: %1; border: 2px solid %2; border-radius: 8px; }")
                    .arg(theme::PANEL, theme::BORDER));
            auto* face_card_layout = new QVBoxLayout(face_card);
            face_card_layout->setContentsMargins(8, 8, 8, 8);
            face_card_layout->setSpacing(8);
            auto* face_image = new QLabel(face_card);
            face_image->setFixedSize(120, 120);
            const QPixmap pixmap = loadPixmapSafe(
                QString::fromStdString(unique_faces[i].face_photo_path));
            if (!pixmap.isNull())
                face_image->setPixmap(pixmap.scaled(
                    120, 120, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            face_image->setAlignment(Qt::AlignCenter);
            face_image->setStyleSheet(QString("background: %1; border: none; border-radius: 6px;")
                                          .arg(theme::FIELD));
            face_card_layout->addWidget(face_image);
            auto* face_index = new QLabel(QStringLiteral("人脸 %1").arg(i + 1), face_card);
            face_index->setAlignment(Qt::AlignCenter);
            face_index->setStyleSheet(QString("color: %1; border: none; font-size:%2px;")
                                          .arg(theme::TEXT_MUTED)
                                          .arg(theme::FS_HINT));
            face_card_layout->addWidget(face_index);
            faces_grid->addWidget(face_card, face_row, face_column);
            if (++face_column >= 5) {
                face_column = 0;
                ++face_row;
            }
        }
        faces_scroll->setWidget(faces_container);
        faces_layout->addWidget(faces_scroll);
        tab_widget->addTab(faces_tab, QStringLiteral("唯一人脸 (%1)").arg(unique_faces.size()));
    }
    
    main_layout->addWidget(tab_widget, 1);
    
    // 底部按钮
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->addStretch();
    
    auto* export_btn = new QPushButton("导出报告", detail_dialog);
    export_btn->setMinimumSize(120, 44);
    export_btn->setStyleSheet(
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 999px;"
        "  padding: 0 24px;"
        "  font-size:%3px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: %4; }")
        .arg(theme::SUCCESS, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SUCCESS_HOVER));
    connect(export_btn, &QPushButton::clicked, [this, task_id]() {
        exportTaskReport(task_id);
    });
    bottom_layout->addWidget(export_btn);
    
    auto* close_btn = new QPushButton("关闭", detail_dialog);
    close_btn->setMinimumSize(120, 44);
    close_btn->setStyleSheet(
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 999px;"
        "  padding: 0 24px;"
        "  font-size:%3px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: %4; }")
        .arg(theme::HOVER, theme::TEXT)
        .arg(theme::FS_CARD)
        .arg(theme::BORDER));
    connect(close_btn, &QPushButton::clicked, detail_dialog, &QWidget::close);
    bottom_layout->addWidget(close_btn);
    
    main_layout->addLayout(bottom_layout);
    
    detail_dialog->show();
}

// 导出纯文本报告：仅汇总任务元信息（人数/状态），不含人脸图片；
// 显式 setCodec("UTF-8") 防止板端默认编解码导致中文乱码
void RollCallWidget::exportTaskReport(int task_id) {
    Task task = service_->getTaskInfo(task_id);
    if (task.id == 0) {
        QMessageBox::warning(this, "错误", "任务不存在");
        return;
    }
    
    QString default_filename = QString("%1_报告.txt").arg(QString::fromStdString(task.name));
    QString save_path = QFileDialog::getSaveFileName(
        this, "导出报告", default_filename, "文本文件 (*.txt)");
    
    if (save_path.isEmpty()) {
        return;
    }
    
    // 生成报告
    QString report;
    QTextStream stream(&report);
    
    stream << "========================================\n";
    stream << "         点名签到任务报告\n";
    stream << "========================================\n\n";
    
    stream << "任务名称: " << QString::fromStdString(task.name) << "\n";
    stream << "创建时间: " << QString::fromStdString(task.create_time) << "\n";
    stream << "登记人数: " << task.registered_count << "\n";
    if (task.is_cancelled)
        stream << "注销人数: " << task.cancelled_count << "\n";
    stream << "任务状态: " << (task.is_cancelled ? "已注销" : "处理中") << "\n\n";
    
    stream << "========================================\n";
    stream << "报告生成时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    stream << "========================================\n";
    
    // 保存到文件
    QFile file(save_path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out.setCodec("UTF-8");
        out << report;
        file.close();
        QMessageBox::information(this, "成功", QString("报告已导出到:\n%1").arg(save_path));
    } else {
        QMessageBox::critical(this, "错误", "导出报告失败");
    }
}
