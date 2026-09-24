// 文件：equipment_detail_dialog.cpp
// 职责：设备盘点任务详情对话框实现（caichao 分支合入），构造时同步取数建表，模态展示
#include "equipment_detail_dialog.h"
#include "theme.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

#include <algorithm>
#include <map>
#include <utility>

#include "../utils/qt_image_utils.h"

EquipmentDetailDialog::EquipmentDetailDialog(
    int task_id, std::shared_ptr<EquipmentInventoryService> service, QWidget* parent)
    : QDialog(parent), task_id_(task_id), service_(std::move(service)) {
    setupUi();
    loadData();
}

// 固定单元格：420x260 内等比缩放；libjpeg ABI 冲突风险统一由
// loadPixmapSafe 兜底（解码失败显示"图片无法加载"而非崩溃）
QWidget* EquipmentDetailDialog::imageWidget(const std::string& path, QWidget* parent) {
    auto* image = new QLabel(parent);
    image->setMinimumSize(300, 210);
    image->setAlignment(Qt::AlignCenter);
    image->setStyleSheet(QString("background:%1; border:1px solid %2;").arg(theme::FIELD, theme::HOVER));
    const QPixmap pixmap = loadPixmapSafe(QString::fromUtf8(path.c_str()));
    if (!pixmap.isNull())
        image->setPixmap(pixmap.scaled(420, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        image->setText(QStringLiteral("图片无法加载"));
    return image;
}

QString EquipmentDetailDialog::countsText(
    const std::vector<EquipmentDetectionRecord>& detections) {
    std::map<std::string, int> counts;
    for (const auto& detection : detections) ++counts[detection.label];
    return countsText(counts);
}

QString EquipmentDetailDialog::countsText(const std::map<std::string, int>& counts) {
    if (counts.empty()) return QStringLiteral("未检测到目标");
    QStringList parts;
    for (const auto& item : counts)
        parts << QStringLiteral("%1: %2").arg(QString::fromUtf8(item.first.c_str())).arg(item.second);
    return parts.join(QStringLiteral("\n"));
}

// ⚠ 遗留死代码警示：photoCell 在当前实现中没有任何调用点
// （loadData 已改为逐格内联 imageWidget + counts 组合），保留仅供未来复用参考。
QWidget* EquipmentDetailDialog::photoCell(const EquipmentPhotoRecord& photo, QWidget* parent) const {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(imageWidget(photo.processed_photo_path, cell));
    const auto detections = service_->getDetections(photo.id);
    auto* counts = new QLabel(countsText(detections), cell);
    counts->setAlignment(Qt::AlignCenter);
    counts->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:600;")
                              .arg(theme::TEXT)
                              .arg(theme::FS_CARD));
    layout->addWidget(counts);
    return cell;
}

void EquipmentDetailDialog::setupUi() {
    setWindowTitle(QStringLiteral("设备盘点任务详情"));
    resize(1500, 950);
    setMinimumSize(1100, 700);
    setStyleSheet(QString("QDialog { background:%1; color:%2; }").arg(theme::BG, theme::TEXT));
}

/**
 * @brief 构造即取数并组装界面（在 GUI 线程同步完成，数据为一次性快照）
 *
 * getPhotos(task_id_, phase)：phase 0=登记照片、1=注销照片。
 * 表头统计先遍历所有照片逐张 getDetections()（N+1 查询，照片量小可接受），
 * 再按任务状态选布局：
 *   - 未注销：2 列表格（后处理图 420x260 缩放 + 标签数量），行高 300；
 *   - 已注销：4 列表格，登记/注销并排对照，行数取两侧较大值，
 *     缺侧留空单元格（行仍高 300 保证视觉对齐）。
 * 底部"关闭"按钮接 accept()：模态 exec() 返回后调用方栈上对象自动析构。
 */
void EquipmentDetailDialog::loadData() {
    const Task task = service_->getTaskInfo(task_id_);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QString::fromUtf8(task.name.c_str()), this);
    title->setStyleSheet(theme::text(theme::TEXT, theme::FS_PAGE, true));
    header->addWidget(title);
    header->addSpacing(24);
    const auto registration = service_->getPhotos(task_id_, 0);
    const auto cancellation = service_->getPhotos(task_id_, 1);
    std::map<std::string, int> registration_counts;
    std::map<std::string, int> cancellation_counts;
    for (const auto& photo : registration)
        for (const auto& detection : service_->getDetections(photo.id)) ++registration_counts[detection.label];
    for (const auto& photo : cancellation)
        for (const auto& detection : service_->getDetections(photo.id)) ++cancellation_counts[detection.label];
    if (task.is_cancelled) {
        auto* totals = new QWidget(this);
        auto* totals_layout = new QHBoxLayout(totals);
        totals_layout->setContentsMargins(0, 0, 0, 0);
        totals_layout->setSpacing(28);

        auto* registration_total = new QLabel(
            QStringLiteral("登记标签总数\n%1").arg(countsText(registration_counts)), totals);
        auto* cancellation_total = new QLabel(
            QStringLiteral("注销标签总数\n%1").arg(countsText(cancellation_counts)), totals);
        for (auto* label : {registration_total, cancellation_total}) {
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(QString("font-size:%1px; font-weight:700; color:%2;"
                                         "padding:4px 12px;")
                                     .arg(theme::FS_PAGE)
                                     .arg(theme::SUCCESS));
        }
        cancellation_total->setStyleSheet(QString("font-size:%1px; font-weight:700; color:%2; padding:4px 12px;")
                                              .arg(theme::FS_PAGE)
                                              .arg(theme::WARNING));
        totals_layout->addWidget(registration_total);
        totals_layout->addWidget(cancellation_total);
        header->addWidget(totals);
    } else {
        auto* total = new QLabel(
            QStringLiteral("标签总数\n%1").arg(countsText(registration_counts)), this);
        total->setAlignment(Qt::AlignCenter);
        total->setStyleSheet(theme::text(theme::SUCCESS, theme::FS_PAGE, true));
        header->addWidget(total);
    }
    header->addStretch();
    root->addLayout(header);

    auto* table = new QTableWidget(this);
    if (!task.is_cancelled) {
        table->setColumnCount(2);
        table->setHorizontalHeaderLabels({QStringLiteral("后处理图片"), QStringLiteral("标签数量")});
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->setRowCount(static_cast<int>(registration.size()));
        for (int i = 0; i < static_cast<int>(registration.size()); ++i) {
            const auto detections = service_->getDetections(registration[i].id);
            table->setCellWidget(i, 0, imageWidget(registration[i].processed_photo_path, table));
            auto* counts = new QLabel(countsText(detections), table);
            counts->setAlignment(Qt::AlignCenter);
            counts->setStyleSheet(QString("font-size:%1px; font-weight:600; color:%2;")
                                      .arg(theme::FS_CARD)
                                      .arg(theme::TEXT));
            table->setCellWidget(i, 1, counts);
            table->setRowHeight(i, 300);
        }
    } else {
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({QStringLiteral("登记后处理图片"), QStringLiteral("登记标签数量"),
                                          QStringLiteral("注销后处理图片"), QStringLiteral("注销标签数量")});
        for (int i = 0; i < 4; ++i) table->horizontalHeader()->setSectionResizeMode(i, QHeaderView::Stretch);
        const int rows = static_cast<int>(std::max(registration.size(), cancellation.size()));
        table->setRowCount(rows);
        for (int i = 0; i < rows; ++i) {
            if (i < static_cast<int>(registration.size())) {
                const auto detections = service_->getDetections(registration[i].id);
                table->setCellWidget(i, 0, imageWidget(registration[i].processed_photo_path, table));
                auto* counts = new QLabel(countsText(detections), table);
                counts->setAlignment(Qt::AlignCenter);
                table->setCellWidget(i, 1, counts);
            }
            if (i < static_cast<int>(cancellation.size())) {
                const auto detections = service_->getDetections(cancellation[i].id);
                table->setCellWidget(i, 2, imageWidget(cancellation[i].processed_photo_path, table));
                auto* counts = new QLabel(countsText(detections), table);
                counts->setAlignment(Qt::AlignCenter);
                table->setCellWidget(i, 3, counts);
            }
            table->setRowHeight(i, 300);
        }
    }
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    root->addWidget(table, 1);

    auto* close = new QPushButton(QStringLiteral("关闭"), this);
    close->setMinimumSize(120, 42);
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(close);
    root->addLayout(buttons);
}
