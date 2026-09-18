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

private:
    void setupUi();
    void loadScreenshot();

    AlarmRecord alarm_;
    QLabel *imageLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};

#endif // ALARM_DETAIL_DIALOG_H
