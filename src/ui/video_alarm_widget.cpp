#include "video_alarm_widget.h"
#include "alarm_detail_dialog.h"
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QDateTime>
#include <QFile>
#include <QCursor>
#include <QMessageBox>
#include <QScreen>
#include <QGuiApplication>

VideoAlarmWidget::VideoAlarmWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void VideoAlarmWidget::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 标题
    QLabel *title = new QLabel("报警信息");
    title->setStyleSheet("color: #fff; font-size: 14px; font-weight: bold; padding: 8px 12px; background: #2d2d3d;");
    mainLayout->addWidget(title);

    // 滚动区域
    scrollArea_ = new QScrollArea();
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setStyleSheet("QScrollArea { background: #1e1e2e; border: none; }");

    contentWidget_ = new QWidget();
    contentWidget_->setStyleSheet("background: #1e1e2e;");
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(8, 8, 8, 8);
    contentLayout_->setSpacing(8);
    contentLayout_->addStretch();

    scrollArea_->setWidget(contentWidget_);
    mainLayout->addWidget(scrollArea_, 1);
}

void VideoAlarmWidget::refreshAlarms()
{
    // 清除现有内容
    QLayoutItem *item;
    while ((item = contentLayout_->takeAt(0)) != nullptr) {
        if (item->widget()) {
            delete item->widget();
        }
        delete item;
    }

    // 获取报警记录，过滤掉误报
    QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();
    QVector<QPair<AlarmRecord, int>> filteredAlarms;
    for (int i = 0; i < alarms.size(); i++) {
        if (!alarms[i].isFalsePositive) {
            filteredAlarms.append(qMakePair(alarms[i], i));
        }
    }

    // 只显示最近的报警（最多20条）
    const int maxDisplay = 20;
    int total = filteredAlarms.size();
    int shown = qMin(total, maxDisplay);

    for (int i = 0; i < shown; i++) {
        const AlarmRecord &a = filteredAlarms[total - 1 - i].first;
        int originalIndex = filteredAlarms[total - 1 - i].second;
        addAlarmItem(a, originalIndex);
    }

    contentLayout_->addStretch();
}

void VideoAlarmWidget::addAlarmItem(const AlarmRecord &alarm, int originalIndex)
{
    QWidget *itemWidget = new QWidget();
    itemWidget->setStyleSheet("background: #2d2d3d; border-radius: 6px;");
    itemWidget->setFixedHeight(80);

    QHBoxLayout *itemLayout = new QHBoxLayout(itemWidget);
    itemLayout->setContentsMargins(8, 6, 8, 6);
    itemLayout->setSpacing(8);

    // 截图缩略图
    QLabel *imageLabel = new QLabel();
    imageLabel->setFixedSize(64, 48);
    imageLabel->setStyleSheet("background: #1e1e2e; border: 1px solid #444; border-radius: 4px;");
    imageLabel->setAlignment(Qt::AlignCenter);

    if (!alarm.imgPath.isEmpty() && QFile::exists(alarm.imgPath)) {
        QPixmap pix(alarm.imgPath);
        if (!pix.isNull()) {
            QPixmap scaled = pix.scaled(64, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            imageLabel->setPixmap(scaled);
        } else {
            imageLabel->setText("无图");
        }
    } else {
        imageLabel->setText("无图");
    }
    itemLayout->addWidget(imageLabel);

    // 信息区域
    QVBoxLayout *infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);

    QDateTime dt;
    dt.setMSecsSinceEpoch(alarm.timestamp / 1000000);

    QLabel *timeLabel = new QLabel(dt.toString("HH:mm:ss"));
    timeLabel->setStyleSheet("color: #aaa; font-size: 11px;");
    infoLayout->addWidget(timeLabel);

    QLabel *classLabel = new QLabel(QString("%1 (%2%)")
        .arg(alarm.className)
        .arg(alarm.confidence * 100, 0, 'f', 0));
    classLabel->setStyleSheet("color: #fff; font-size: 12px; font-weight: bold;");
    infoLayout->addWidget(classLabel);

    QLabel *channelLabel = new QLabel(QString("通道%1").arg(alarm.channel + 1));
    channelLabel->setStyleSheet("color: #888; font-size: 11px;");
    infoLayout->addWidget(channelLabel);

    itemLayout->addLayout(infoLayout, 1);

    // 操作按钮（详情）
    QPushButton *btnDetail = new QPushButton("详情");
    btnDetail->setStyleSheet(
        "QPushButton { color: #fff; background: #4a6fa5; border-radius: 4px; padding: 3px 8px; font-size: 11px; }"
        "QPushButton:hover { background: #5a8fc5; }"
    );
    connect(btnDetail, &QPushButton::clicked, this, [this, originalIndex]() {
        onAlarmClicked(originalIndex);
    });
    itemLayout->addWidget(btnDetail);

    contentLayout_->addWidget(itemWidget);
}

void VideoAlarmWidget::onAlarmClicked(int index)
{
    QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();
    if (index < 0 || index >= alarms.size()) return;

    AlarmRecord alarm = alarms[index];

    AlarmDetailDialog dlg(alarm, index, this);
    connect(&dlg, &AlarmDetailDialog::alarmMarkedFalsePositive, this, [](int idx) {
        AlarmManager::instance().markAsFalsePositive(idx);
    });
    connect(&dlg, &AlarmDetailDialog::alarmAcknowledged, this, [](int idx) {
        AlarmManager::instance().acknowledgeAlarm(idx);
    });
    dlg.exec();

    refreshAlarms();
    emit alarmUpdated();
}
