#include "alarm_detail_dialog.h"
#include "theme.h"
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

AlarmDetailDialog::AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent)
    : QDialog(parent), alarm_(alarm)
{
    setupUi();
    loadScreenshot();
}

void AlarmDetailDialog::setupUi()
{
    setWindowTitle("报警详情");
    setMinimumSize(480, 520);
    setStyleSheet(QString("background: %1; color: %2;").arg(theme::PANEL, theme::TEXT));

    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);

    // 截图预览区域
    imageLabel_ = new QLabel();
    imageLabel_->setMinimumSize(440, 280);
    imageLabel_->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 4px;")
                                   .arg(theme::BG, theme::BORDER));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setText("无截图");
    mainLay->addWidget(imageLabel_);

    // 信息网格
    QGridLayout *infoLay = new QGridLayout();
    infoLay->setHorizontalSpacing(12);
    infoLay->setVerticalSpacing(8);

    auto addRow = [&](int row, const QString &label, const QString &value) {
        QLabel *lbl = new QLabel(label);
        lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        QLabel *val = new QLabel(value);
        val->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY, true));
        infoLay->addWidget(lbl, row, 0);
        infoLay->addWidget(val, row, 1);
        return val;
    };

    QDateTime dt = QDateTime::fromString(alarm_.alarmTime, Qt::ISODate);

    addRow(0, "时间:", dt.toString("yyyy-MM-dd HH:mm:ss"));
    addRow(1, "通道:", QString("通道 %1").arg(alarm_.channel + 1));
    addRow(2, "类别:", alarm_.className);
    addRow(3, "置信度:", QString("%1%").arg(alarm_.confidence * 100, 0, 'f', 1));
    addRow(4, "ID:", alarm_.id);
    statusLabel_ = addRow(5, "状态:", alarm_.status == "rectified" ? "已确认" : "未确认");
    if (alarm_.status != "rectified") {
        statusLabel_->setStyleSheet(theme::text(theme::WARNING, theme::FS_BODY, true));
    } else {
        statusLabel_->setStyleSheet(theme::text(theme::SUCCESS, theme::FS_BODY, true));
    }

    mainLay->addLayout(infoLay);

    mainLay->addStretch();

    // 按钮区域：确认误报 | 不是误报 | 取消
    QHBoxLayout *btnLay = new QHBoxLayout();
    btnLay->setSpacing(12);

    QPushButton *btnFalse = new QPushButton("确认误报");
    btnFalse->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::DANGER)
            .arg(theme::FS_BODY));
    connect(btnFalse, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkFalsePositive);
    btnLay->addWidget(btnFalse);

    QPushButton *btnNormal = new QPushButton("不是误报");
    btnNormal->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::SUCCESS)
            .arg(theme::FS_BODY));
    connect(btnNormal, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkNormal);
    btnLay->addWidget(btnNormal);

    btnLay->addStretch();

    QPushButton *btnCancel = new QPushButton("取消");
    btnCancel->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::HOVER)
            .arg(theme::FS_BODY));
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
        emit alarmMarkedFalsePositive(alarm_.id);
        accept();
    }
}

void AlarmDetailDialog::onMarkNormal()
{
    emit alarmAcknowledged(alarm_.id);
    accept();
}
