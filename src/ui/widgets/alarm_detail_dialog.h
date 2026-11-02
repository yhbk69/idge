#ifndef ALARM_DETAIL_DIALOG_H
#define ALARM_DETAIL_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include "alarm_manager.h"

class AlarmDetailDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent = nullptr);

signals:
    void alarmMarkedFalsePositive(const QString &alarmId);
    void alarmAcknowledged(const QString &alarmId);

private slots:
    void onMarkFalsePositive();
    void onMarkNormal();

protected:
    // 抓拍图要跟着对话框一起长大：构造时按 label 的当前尺寸缩放一次，等于把图
    // 钉死在最小尺寸上，之后用户/代码再怎么放大对话框，图仍是那一小块。
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupUi();
    void loadScreenshot();
    void rescaleScreenshot();

    AlarmRecord alarm_;
    QPixmap snapshot_;        // 原始抓拍（缩放的唯一来源，不重复读盘）
    QLabel *imageLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};

#endif // ALARM_DETAIL_DIALOG_H
