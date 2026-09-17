#include "alarm_detail_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QScreen>
#include <QGuiApplication>

AlarmDetailDialog::AlarmDetailDialog(const AlarmRecord &alarm, int alarmIndex, QWidget *parent)
    : QDialog(parent), alarm_(alarm), alarmIndex_(alarmIndex)
{
    setupUi();
    loadScreenshot();
}

void AlarmDetailDialog::setupUi()
{
    setWindowTitle("报警详情");
    setMinimumSize(480, 520);
    setStyleSheet("background: #2d2d3d; color: #ccc;");

    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);

    // 截图预览区域
    imageLabel_ = new QLabel();
    imageLabel_->setMinimumSize(440, 280);
    imageLabel_->setStyleSheet("background: #1e1e2e; border: 1px solid #444; border-radius: 4px;");
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setText("无截图");
    mainLay->addWidget(imageLabel_);

    // 信息网格
    QGridLayout *infoLay = new QGridLayout();
    infoLay->setHorizontalSpacing(12);
    infoLay->setVerticalSpacing(8);

    auto addRow = [&](int row, const QString &label, const QString &value) {
        QLabel *lbl = new QLabel(label);
        lbl->setStyleSheet("color: #aaa; font-size: 13px;");
        QLabel *val = new QLabel(value);
        val->setStyleSheet("color: #fff; font-size: 13px; font-weight: bold;");
        infoLay->addWidget(lbl, row, 0);
        infoLay->addWidget(val, row, 1);
        return val;
    };

    QDateTime dt;
    dt.setMSecsSinceEpoch(alarm_.timestamp / 1000000);

    addRow(0, "时间:", dt.toString("yyyy-MM-dd HH:mm:ss"));
    addRow(1, "通道:", QString("通道 %1").arg(alarm_.channel + 1));
    addRow(2, "类别:", alarm_.className);
    addRow(3, "置信度:", QString("%1%").arg(alarm_.confidence * 100, 0, 'f', 1));
    statusLabel_ = addRow(4, "状态:", alarm_.status == "rectified" ? "已确认" : "未确认");
    if (alarm_.status != "rectified") {
        statusLabel_->setStyleSheet("color: #ff9800; font-size: 13px; font-weight: bold;");
    } else {
        statusLabel_->setStyleSheet("color: #4caf50; font-size: 13px; font-weight: bold;");
    }

    mainLay->addLayout(infoLay);

    mainLay->addStretch();

    // 按钮区域：确认误报 | 不是误报 | 取消
    QHBoxLayout *btnLay = new QHBoxLayout();
    btnLay->setSpacing(12);

    QPushButton *btnFalse = new QPushButton("确认误报");
    btnFalse->setStyleSheet(
        "QPushButton { color: #fff; background: #f44336; border-radius: 4px; padding: 8px 20px; font-size: 14px; }"
        "QPushButton:hover { background: #d32f2f; }"
    );
    connect(btnFalse, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkFalsePositive);
    btnLay->addWidget(btnFalse);

    QPushButton *btnNormal = new QPushButton("不是误报");
    btnNormal->setStyleSheet(
        "QPushButton { color: #fff; background: #4caf50; border-radius: 4px; padding: 8px 20px; font-size: 14px; }"
        "QPushButton:hover { background: #388e3c; }"
    );
    connect(btnNormal, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkNormal);
    btnLay->addWidget(btnNormal);

    btnLay->addStretch();

    QPushButton *btnCancel = new QPushButton("取消");
    btnCancel->setStyleSheet(
        "QPushButton { color: #fff; background: #555; border-radius: 4px; padding: 8px 20px; font-size: 14px; }"
        "QPushButton:hover { background: #666; }"
    );
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLay->addWidget(btnCancel);

    mainLay->addLayout(btnLay);
}

void AlarmDetailDialog::loadScreenshot()
{
    if (alarm_.imagePath.isEmpty() || !QFile::exists(alarm_.imagePath)) {
        imageLabel_->setText("无截图");
        return;
    }

    QPixmap pix(alarm_.imagePath);
    if (pix.isNull()) {
        imageLabel_->setText("截图加载失败");
        return;
    }

    QPixmap scaled = pix.scaled(imageLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imageLabel_->setPixmap(scaled);
}

void AlarmDetailDialog::onMarkFalsePositive()
{
    QMessageBox box(QMessageBox::Question, tr("确认误报"), tr("确定将此报警标记为误报？\n标记后状态将变为误报。"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QAbstractButton *okBtn = box.button(QMessageBox::Ok);
    if (okBtn) okBtn->setText(tr("确定"));
    QAbstractButton *cancelBtn = box.button(QMessageBox::Cancel);
    if (cancelBtn) cancelBtn->setText(tr("取消"));
    box.adjustSize();

    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();

    if (box.exec() == QMessageBox::Ok) {
        emit alarmMarkedFalsePositive(alarmIndex_);
        accept();
    }
}

void AlarmDetailDialog::onMarkNormal()
{
    emit alarmAcknowledged(alarmIndex_);
    accept();
}
