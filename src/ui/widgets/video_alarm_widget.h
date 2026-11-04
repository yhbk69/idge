#ifndef VIDEO_ALARM_WIDGET_H
#define VIDEO_ALARM_WIDGET_H

// ============================================================================
// video_alarm_widget.h - 视频监控页右侧的"最近报警"卡片列表面板
// ============================================================================
//
// 【所属模块】src/ui/widgets。与报警数据页（alarm_list_widget）的分工：
//   这里是**内存窗口**（AlarmManager 的最近 1000 条，取最近 kMaxDisplay 条），
//   那里是**库**（全量历史 + 检索）。所以两边数字对不上是正常的：
//   这里回答"刚刚发生了什么"，不回答"一共有多少"。
// 【刷新模型】refreshAlarms() 做**增量**对齐：算出想要的 id 序列 → 与 shownIds_
//   比较 → 只删消失的、只建新增的。全拆重建实测 600~765ms，会把四路 paintGL
//   挤掉 8~10 帧，所以这条路径必须一直保留；判重失败（集合与顺序都没变）时
//   只剩一次 alarms() 拷贝（1~3ms）。
// 【线程】全部 GUI 线程。数据源 AlarmManager::instance().alarms() 内部有锁，
//   返回的是**快照副本**，因此遍历期间不会被别的线程改坏。
// 【卡片是普通 QWidget】不是按钮：整张卡片当点击入口，靠 eventFilter 认
//   "按下-松开且位移≤16px"这一次点击（阈值理由见 .cpp 的 eventFilter 注释）。
// ============================================================================

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

    // 把面板对齐到"最近 kMaxDisplay 条（滤掉误报）"。
    // ⚠ 不可见时直接 return（一次全量重建实测 600~765ms，够挤掉四路 8~10 帧）；
    //   隐藏期间到达的报警由 showEvent 补刷，所以调用方**不需要**判可见性。
    //   外部（frmMain 的 300ms 防抖）只管按时钟调，重复调是廉价的。
    void refreshAlarms();

signals:
    // 用户在本面板里把某条报警处理掉了（详情框里点了确认/误报）之后发一次，
    // 让外部同步其它页与上行。刷新动作本身不触发它——只有"人改了状态"才发。
    void alarmUpdated();

private slots:
    // 打开这条报警的详情框（模态）。按 id 在内存窗口里找，找不到就直接返回：
    // 卡片一定来自同一份 alarms() 快照，所以"找不到"只可能是这条已被裁出 1000 条窗口。
    void onAlarmClicked(const QString &alarmId);

protected:
    // 整张卡片就是入口（原来那颗「详情」钮已撤）：卡片是普通 QWidget，
    // 收不到子控件上的点击，所以子件一律 WA_TransparentForMouseEvents，
    // 再在这里把按下-松开的一次完整点击认成"看这条的详情"。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // 建标题 + QScrollArea + contentWidget 的骨架，并把**条目样式集中**刷在这一次
    // setStyleSheet 上（条目内控件只用 objectName 命中规则）。
    // 只在构造里调一次：条目是运行期反复增删的，样式表每重设一次就要重新 polish
    // 整棵子树，20 条 ×3 控件的重复解析是这个面板最初的耗时来源之一。
    void setupUi();
    // 构建一条报警项并登记进 items_，但不加入布局（由 refreshAlarms 决定落位）
    QWidget *createAlarmItem(const AlarmRecord &alarm);
    // 回到本页补一次刷：隐藏期间 refreshAlarms 全是直接 return 的，不补就永远落后。
    void showEvent(QShowEvent *event) override;

    // 面板最多显示最近 20 条：这是"侧栏一屏能看几条"（实测 ~12 条 72px 卡片）
    // 再留一点滚动余量定的，不是内存窗口的容量（那是 AlarmManager 的 1000 条）。
    // 调大它会线性抬高每次刷新的建 widget 数量，先看 perfProbe 的 created 计数再动。
    static const int kMaxDisplay = 20;

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
