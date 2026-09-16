#ifndef VIDEO_ALARM_WIDGET_H
#define VIDEO_ALARM_WIDGET_H

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QVector>
#include "alarm_manager.h"

class VideoAlarmWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoAlarmWidget(QWidget *parent = nullptr);

    void refreshAlarms();

signals:
    void alarmRemoved(int index);

private slots:
    void onAlarmClicked(int index);
    void onRemoveClicked(int index);

private:
    void setupUi();
    void addAlarmItem(const AlarmRecord &alarm, int originalIndex);

    QScrollArea *scrollArea_ = nullptr;
    QWidget *contentWidget_ = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;
};

#endif // VIDEO_ALARM_WIDGET_H
