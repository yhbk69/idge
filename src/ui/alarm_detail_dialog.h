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
    explicit AlarmDetailDialog(const AlarmRecord &alarm, int alarmIndex, QWidget *parent = nullptr);

signals:
    void alarmRemoved(int index);

private slots:
    void onMarkFalsePositive();

private:
    void setupUi();
    void loadScreenshot();

    AlarmRecord alarm_;
    int alarmIndex_;
    QLabel *imageLabel_ = nullptr;
    QLabel *timeLabel_ = nullptr;
    QLabel *channelLabel_ = nullptr;
    QLabel *classLabel_ = nullptr;
    QLabel *confidenceLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};

#endif // ALARM_DETAIL_DIALOG_H
