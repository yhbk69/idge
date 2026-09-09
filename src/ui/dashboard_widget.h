#ifndef DASHBOARD_WIDGET_H
#define DASHBOARD_WIDGET_H

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
#include <QMap>
#include <QScrollArea>

class DashboardWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DashboardWidget(QWidget *parent = nullptr);

    void refreshStats();

private slots:
    void onTimer();

private:
    void setupUi();
    QWidget *createCard(const QString &title, const QString &unit,
                        QLabel **valueLabel, QLabel **descLabel);

    QLabel *lblDetections_ = nullptr;
    QLabel *lblDetectionsDesc_ = nullptr;
    QLabel *lblAlarms_ = nullptr;
    QLabel *lblAlarmsDesc_ = nullptr;
    QLabel *lblChannels_ = nullptr;
    QLabel *lblChannelsDesc_ = nullptr;
    QLabel *lblUptime_ = nullptr;
    QLabel *lblUptimeDesc_ = nullptr;

    QLabel *lblCpu_ = nullptr;
    QLabel *lblMem_ = nullptr;
    QLabel *lblTemp_ = nullptr;
    QLabel *lblNpu_ = nullptr;

    QWidget *classStatsWidget_ = nullptr;
    QVBoxLayout *classStatsLayout_ = nullptr;
    QScrollArea *classStatsScroll_ = nullptr;

    QTimer refreshTimer_;
    qint64 startTime_ = 0;
};

#endif // DASHBOARD_WIDGET_H
