#ifndef VIDEO_ALARM_WIDGET_H
#define VIDEO_ALARM_WIDGET_H

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QVector>
#include <QHash>
#include <QStringList>
#include "alarm_manager.h"

class VideoAlarmWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoAlarmWidget(QWidget *parent = nullptr);

    void refreshAlarms();

signals:
    void alarmUpdated();

private slots:
    void onAlarmClicked(const QString &alarmId);

protected:
    // 整张卡片就是入口（原来那颗「详情」钮已撤）：卡片是普通 QWidget，
    // 收不到子控件上的点击，所以子件一律 WA_TransparentForMouseEvents，
    // 再在这里把按下-松开的一次完整点击认成"看这条的详情"。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    // 构建一条报警项并登记进 items_，但不加入布局（由 refreshAlarms 决定落位）
    QWidget *createAlarmItem(const AlarmRecord &alarm);
    void showEvent(QShowEvent *event) override;

    static const int kMaxDisplay = 20;  // 面板最多显示最近 20 条

    QScrollArea *scrollArea_ = nullptr;
    QWidget *contentWidget_ = nullptr;
    QVBoxLayout *contentLayout_ = nullptr;

    // 增量刷新用：报警 id -> 已创建的条目 widget，以及当前显示顺序（新->旧）。
    // 面板每秒可能被节流定时器触发刷新好几次，全拆重建会把 GUI 线程占住几百毫秒，
    // 因此只对新报警建 widget、对消失的报警删 widget，其余原地挪位置。
    QHash<QString, QWidget *> items_;
    QStringList shownIds_;
};

#endif // VIDEO_ALARM_WIDGET_H
