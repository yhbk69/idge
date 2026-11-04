#ifndef ALARM_DETAIL_DIALOG_H
#define ALARM_DETAIL_DIALOG_H

// ============================================================================
// alarm_detail_dialog.h - 单条报警的详情框（抓拍图 + 判误报的入口）
// ============================================================================
//
// 【所属模块】src/ui/widgets。两个入口共用它：报警数据页的行内「详情」/双击，
//   以及视频监控页右侧那张卡片。传进来的 AlarmRecord 由调用方负责取到——
//   本框**不回查库、也不读 AlarmManager**，只把这份快照渲染出来。
// 【它不写数据库】两个按钮只 emit 信号，改状态的是持有 AlarmManager 的调用方
//   （见 video_alarm_widget.cpp / alarm_list_widget.cpp 里的 connect）。
//   这样这个框就能被任何一侧复用，也避免对话框自己去动单例。
// 【exec() 语义】栈上构造 + exec()：模态期间 GUI 线程继续跑事件循环
//   （四路 paintGL、防抖定时器都在走），所以本框里的耗时点只有抓拍图解码一次。
// 【视频行是尽力而为的快照】片段在报警后约 2 秒才落盘、videoPath 由 AlarmManager
//   事后回填且**不发信号**，本框只在构造时读一次：刚报警就点开可能显示"未生成"。
// ============================================================================

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include "alarm_manager.h"

class AlarmDetailDialog : public QDialog
{
    Q_OBJECT
public:
    // alarm 必须是调用方拿到的**那一份**记录（按值存进 alarm_，之后不跟随外部变化）。
    // 构造即建 UI 并读一次盘上的抓拍图；此时不检查 videoPath 是否"还没落盘"，
    // 所以现在打开太快看到的"未生成"不代表片段不存在——隔两秒重开一次即可。
    explicit AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent = nullptr);

signals:
    // 用户在框里判"这条是误报"/"这条不是误报"。参数是本条的 id
    // （来自调用方给的那份记录，报警数据页传的可能是库里的行）。
    // 两个按钮都是"emit 完立刻 accept()"，所以调用方拿到信号时本框已经关闭。
    void alarmMarkedFalsePositive(const QString &alarmId);
    void alarmAcknowledged(const QString &alarmId);

private slots:
    // 「确认误报」：先弹一层置顶确认框（判错的方向不可逆，值得多一次点击），
    // 只有确认框回 Ok 才 emit alarmMarkedFalsePositive。取消则什么都不做、框也不关。
    void onMarkFalsePositive();
    // 「不是误报」：不二次确认，直接 emit alarmAcknowledged + accept()。
    void onMarkNormal();

protected:
    // 抓拍图要跟着对话框一起长大：构造时按 label 的当前尺寸缩放一次，等于把图
    // 钉死在最小尺寸上，之后用户/代码再怎么放大对话框，图仍是那一小块。
    void resizeEvent(QResizeEvent *event) override;

private:
    // 组 UI：预览区 + 信息网格 + 底部三颗钮。
    // 只在构造里调一次，尺寸档（720x620 下限 / 1240x1060 上限）按**父窗所在屏**算，
    // 不用 primaryScreen——双屏调试机上 primary 是那台没有触摸的外接屏。
    void setupUi();
    // 读一次盘上的抓拍图进 snapshot_。失败分两种（没路径/文件不在、图本身解不开），
    // 两种都只把 label 换成中文提示，不弹窗、不 return 错误码——详情框宁可少一张图。
    void loadScreenshot();
    // 按 imageLabel_ 当前尺寸重贴一次图（源永远是 snapshot_，不逐级劣化）。
    // 可以从任何地方调；snapshot_ 为空时直接返回。
    void rescaleScreenshot();

    AlarmRecord alarm_;
    QPixmap snapshot_;        // 原始抓拍（缩放的唯一来源，不重复读盘）
    QLabel *imageLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
};

#endif // ALARM_DETAIL_DIALOG_H
