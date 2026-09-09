#ifndef ALARM_LIST_WIDGET_H
#define ALARM_LIST_WIDGET_H

#include <QWidget>
#include <QTableWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>

class AlarmListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AlarmListWidget(QWidget *parent = nullptr);

    void refreshTable();

private slots:
    void onRefresh();
    void onAcknowledge();
    void onClear();
    void onStatsUpdated();

private:
    void setupUi();

    QTableWidget *table_ = nullptr;
    QComboBox *filterChannel_ = nullptr;
    QComboBox *filterClass_ = nullptr;
    QLabel *lblTotal_ = nullptr;
    QLabel *lblUnack_ = nullptr;
};

#endif // ALARM_LIST_WIDGET_H
