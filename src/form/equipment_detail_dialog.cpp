#include "equipment_detail_dialog.h"

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

QWidget* EquipmentDetailDialog::imageWidget(const std::string& path, QWidget* parent) {
    auto* image = new QLabel(parent);
    image->setMinimumSize(300, 210);
    image->setAlignment(Qt::AlignCenter);
    image->setStyleSheet(QStringLiteral("background:#0A0D12; border:1px solid #1E2636;"));
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

QWidget* EquipmentDetailDialog::photoCell(const EquipmentPhotoRecord& photo, QWidget* parent) const {
    auto* cell = new QWidget(parent);
    auto* layout = new QVBoxLayout(cell);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(imageWidget(photo.processed_photo_path, cell));
    const auto detections = service_->getDetections(photo.id);
    auto* counts = new QLabel(countsText(detections), cell);
    counts->setAlignment(Qt::AlignCenter);
    counts->setStyleSheet(QStringLiteral("color:#E5E7EB; font-size:16px; font-weight:600;"));
    layout->addWidget(counts);
    return cell;
}

void EquipmentDetailDialog::setupUi() {
    setWindowTitle(QStringLiteral("设备盘点任务详情"));
    resize(1500, 950);
    setMinimumSize(1100, 700);
    setStyleSheet(QStringLiteral("QDialog { background:#05070C; color:#E5E7EB; }"));
}

void EquipmentDetailDialog::loadData() {
    const Task task = service_->getTaskInfo(task_id_);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(QString::fromUtf8(task.name.c_str()), this);
    title->setStyleSheet(QStringLiteral("font-size:24px; font-weight:700; color:#E5E7EB;"));
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
            label->setStyleSheet(QStringLiteral(
                "font-size:20px; font-weight:700; color:#6EE7B7;"
                "padding:4px 12px;"));
        }
        cancellation_total->setStyleSheet(QStringLiteral(
            "font-size:20px; font-weight:700; color:#E9A568; padding:4px 12px;"));
        totals_layout->addWidget(registration_total);
        totals_layout->addWidget(cancellation_total);
        header->addWidget(totals);
    } else {
        auto* total = new QLabel(
            QStringLiteral("标签总数\n%1").arg(countsText(registration_counts)), this);
        total->setAlignment(Qt::AlignCenter);
        total->setStyleSheet(QStringLiteral("font-size:20px; font-weight:700; color:#6EE7B7;"));
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
            counts->setStyleSheet(QStringLiteral("font-size:17px; font-weight:600; color:#E5E7EB;"));
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
