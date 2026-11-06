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
#include <QStringList>
#include "alarm_manager.h"

class QButtonGroup;
class QLineEdit;
class QCloseEvent;
class QVBoxLayout;
class QStackedLayout;
class QHBoxLayout;
class GLVideoWidget;
class FFmpegVideoDecoder;

namespace ui {

/**
 * @brief 详情框「提交处置」的落库收口（本框调用；两个入口只接 alarmDisposed 刷列表）
 * @param optionId §4.1 处置选项 id（1..4），落 alarms.dispose_option_id
 * @param photoPathsJson 相对媒体根路径的 JSON 数组文本，落 alarms.dispose_photo
 * @return false = 库里没写成，err 带中文原因（此时本框不关、照片也不删，用户可直接重试）
 *
 * 处置人取当前扫码登录会话的工号/姓名（没登录就留空，与报警行上的施工信息同一口径）；
 * 时间不给，让 DAO 用本机此刻——详情框点提交就是"现在做完的"。
 *
 * 为什么单独一个函数：disposeAlarmById 有九个入参、两个数据来源（会话 + 表单），
 * 而且必须走它而不是 AlarmDAO::dispose() —— 它同时动内存窗口里那份同 id 副本，
 * 少了那一步，关机时 syncToDatabase() 的整批 INSERT OR REPLACE 会把这条处置打回 pending。
 * 收在一处才不会被抄错。
 */
bool commitAlarmDisposal(const QString &alarmId, const QString &measure, const QString &remark,
                         int optionId, const QString &photoPathsJson, QString *err = nullptr);

}  // namespace ui

class AlarmDetailDialog : public QDialog
{
    Q_OBJECT
public:
    // alarm 必须是调用方拿到的**那一份**记录（按值存进 alarm_，之后不跟随外部变化）。
    // 构造即建 UI 并读一次盘上的抓拍图；此时不检查 videoPath 是否"还没落盘"，
    // 所以现在打开太快看到的"未生成"不代表片段不存在——隔两秒重开一次即可。
    explicit AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent = nullptr);
    // 片段解码器是本框自己起的另一路流（PreviewOnly，不碰 NPU）：关框必须停它，
    // 否则解码线程还在跑、还在往已经析构的 GL 控件发帧。
    ~AlarmDetailDialog() override;

signals:
    // 用户在框里判"这条是误报"/"这条不是误报"。参数是本条的 id
    // （来自调用方给的那份记录，报警数据页传的可能是库里的行）。
    // 两个按钮都是"emit 完立刻 accept()"，所以调用方拿到信号时本框已经关闭。
    void alarmMarkedFalsePositive(const QString &alarmId);
    void alarmAcknowledged(const QString &alarmId);

    // 「提交处置」已经落库成功，调用方刷新列表即可（与上面两条信号同一口径）。
    // 为什么不像它们那样把参数发出去让调用方改库：处置是一次"表单 + 照片文件"的写，
    // 库里没写成，本框就不能把照片交出去（否则盘上留下一组没人认领的 jpg）。
    // 所以真正调 ui::commitAlarmDisposal() 的是本框，失败就地报错、表单留着让用户重试。
    void alarmDisposed(const QString &alarmId);

private slots:
    // 「确认误报」：先弹一层置顶确认框（判错的方向不可逆，值得多一次点击），
    // 只有确认框回 Ok 才 emit alarmMarkedFalsePositive。取消则什么都不做、框也不关。
    void onMarkFalsePositive();
    // 「不是误报」：不二次确认，直接 emit alarmAcknowledged + accept()。
    void onMarkNormal();
    // 「拍照」：调起拍照框（只从实时摄像头拍，没有相册），回来的路径加进本条处置。
    void onTakeDisposePhoto();
    // 「提交处置」：一条都没选就不放行——中文措施要落 dispose_result，
    // 空着写进去等于造一条"已处置但没写措施"的行，§4.2 那边也按必填拒过。
    void onSubmitDispose();

protected:
    // 抓拍图要跟着对话框一起长大：构造时按 label 的当前尺寸缩放一次，等于把图
    // 钉死在最小尺寸上，之后用户/代码再怎么放大对话框，图仍是那一小块。
    void resizeEvent(QResizeEvent *event) override;
    // 没提交就关框（取消、ESC、窗口 ✕）：本次现拍但没交的照片从盘上删掉。
    // 留着就是"盘上有照片、库里没这条处置"，而 data/alarms/dispose/ 不归按天清理管，
    // 会一直攒到把卡写满。三条关闭路径都要收口，所以 reject() 与 closeEvent() 都接。
    void closeEvent(QCloseEvent *event) override;
    void reject() override;
    // 视频那一格的封面与画面都靠"点一下"才有动作，而 QLabel / GLVideoWidget 自己
    // 没有 clicked 信号——接事件过滤器比给两个控件各派一个子类便宜。
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    // 处置区：终态行（已处置/已误报）只把既有内容显示出来、「提交」那颗钮不铺
    // （§3.7 终态不回退，界面上不该还能再提交一次）；非终态才铺可填的表单。
    void setupDisposeArea(QVBoxLayout *mainLay);
    // 本次现拍但没提交的照片从盘上删掉，删完清空列表。
    void discardUnsubmittedPhotos();
    // 右上那一格：封面（默认）/ 播放画面两层，点封面才起第二路解码。
    void setupVideoPane(QHBoxLayout *topRow);
    // 能播的那份是同名 .mp4（库里 video_path 存的是 Annex-B 裸流，浏览器和 Qt 都放不了）。
    // 不存在/没回填 → 空串，封面就显示"未生成/已清理"并且点了没反应。
    QString playableClipPath() const;
    void startClipPlayback();
    // 回到封面那层并停掉第二路解码（放完、出错、关框三条路都收到这里）。
    void stopClipPlayback();

    AlarmRecord alarm_;
    QPixmap snapshot_;        // 原始抓拍（缩放的唯一来源，不重复读盘）
    QLabel *imageLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;

    // ===== 处置表单（只有非终态行才可填）=====
    QButtonGroup *disposeGroup_ = nullptr;
    QLineEdit *disposeRemark_ = nullptr;
    QLabel *photoCountLabel_ = nullptr;
    QPushButton *snapButton_ = nullptr;
    QWidget *disposeForm_ = nullptr;      ///< 非空 = 这一条还能提交处置
    QStringList disposePhotos_;           ///< 已计入本条处置的照片（绝对路径）
    QStringList disposePhotosTaken_;      ///< 本次在框里现拍的（没提交就要删的那批）
    bool submitted_ = false;              ///< 走过「提交处置」才保留照片

    // ===== 片段播放（右上那一格）=====
    QWidget *videoPane_ = nullptr;
    QStackedLayout *videoStack_ = nullptr;
    QWidget *videoPoster_ = nullptr;      ///< 第 0 层：封面图 + "点击播放"，点了才起播
    QLabel *clipPosterImage_ = nullptr;   ///< 封面里那张图（缩放目标，源仍是 snapshot_）
    QLabel *clipHint_ = nullptr;          ///< 封面下面那行字（可播/未生成/失败原因）
    GLVideoWidget *clipView_ = nullptr;   ///< 第 1 层：真在放的那一帧
    FFmpegVideoDecoder *clipDecoder_ = nullptr;
};

#endif // ALARM_DETAIL_DIALOG_H
