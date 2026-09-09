#ifndef ALARM_LIST_WIDGET_H
#define ALARM_LIST_WIDGET_H

#include <QWidget>
#include <QTableWidget>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QTimer>

class AlarmListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit AlarmListWidget(QWidget *parent = nullptr);

    void refreshTable();

protected:
    void showEvent(QShowEvent *event) override;   // 页面切到可见时刷新一次

private slots:
    void onRefresh();
    void onAcknowledge();
    void onClear();
    void onStatsUpdated();
    void onOpenDir();
    void onRowDoubleClicked(int row, int column);
    void onDebouncedRefresh();   // 防抖：多条报警合并成一次整表刷新

private:
    void setupUi();

    QTableWidget *table_ = nullptr;
    QComboBox *filterChannel_ = nullptr;
    QComboBox *filterClass_ = nullptr;
    QLabel *lblTotal_ = nullptr;
    QLabel *lblUnack_ = nullptr;
    QPushButton *snapBtn_ = nullptr;    // 截图开关按钮
    QTimer *refreshTimer_ = nullptr;    // 防抖刷新定时器
};

#endif // ALARM_LIST_WIDGET_H
