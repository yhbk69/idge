// ============================================================================
// alarm_detail_dialog.cpp - 单条报警详情框实现
// ============================================================================
//
// 【所属模块】src/ui/widgets。这个框存在的唯一理由是"判是不是误报"，
//   所以版面把绝大部分高度让给抓拍预览（setStretch 3:1），信息网格只占一小条。
// 【渲染口径的两处外部依赖】
//   - 施工信息按 construction_info 的键**逐行**渲染，标签/顺序来自
//     ConstructionInfo::fieldsFromJson()，与 ConstructionDetailDialog 同一份口径
//     ⇒ 协议加字段时这里不用改。
//   - 配色/字阶一律走 theme::，不写死色值（见 src/ui/core/theme.h 的第一条约定）。
// 【只读不改】状态变化一律 emit 信号给调用方，本框自己不写库。唯一的例外是
//   文件末尾那个 ui::commitAlarmDisposal()：它是"提交处置"的落库收口，放在这个
//   文件里只为让两个调用点不必各抄一遍八字段的 disposeAlarmById 调用。
// ============================================================================
#include "alarm_detail_dialog.h"
#include "alarm_labels.h"
#include "capture_dialog.h"
#include "construction_session.h"
#include "ffmpeg_video_decoder.h"
#include "gl_video_widget.h"
#include "qt_image_utils.h"
#include "qthelper.h"
#include "runtime_paths.h"
#include "theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QStackedLayout>
#include <QEvent>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHash>
#include <QPixmap>
#include <QScreen>
#include <QGuiApplication>
#include <QVector>
#include <QButtonGroup>
#include <QRadioButton>
#include <QLineEdit>
#include <QCloseEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace {

// v1 §4.2 的两条上限，Pad 提交路径（pad_http_alarms.cpp）用的是同一对数：
// 本机也能拍 6 张、单张随大小的话，同一条报警在两端就变成两套规则了。
constexpr int kMaxDisposePhotos = 5;
constexpr qint64 kMaxDisposePhotoBytes = 5LL * 1024 * 1024;

/// 媒体根的绝对路径：Pad 出证据地址、片段落盘、整改照片共用这一棵树
QString mediaRootAbs()
{
    return QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
}

/// alarmId 能不能拿来拼路径。库里那列是本机生成的 32 位十六进制 UUID，但历史/联调数据
/// 可能有别的写法，所以判据与 Pad 那条 POST /api/alarms/{id}/dispose 完全一致
/// （[A-Za-z0-9_-]{1,64}）：**两端不同步就等于留了一扇只有本机没有的门**——
/// 详情框把 id 直接拼进照片目录，一个带 ../ 的 id 就能把照片写到媒体根外面去。
bool isSafeAlarmIdForPath(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return id.size() <= 64 && re.match(id).hasMatch();
}

/// dispose_photo 那列里已有几张照片。老行可能是**单张裸路径**（这一列最初的口径就是
/// 单张），所以两种形状都认，判据与 pad_wire::disposeObjectOf 同一句（看首字符，不 try-parse）。
int disposePhotoCountFromJson(const QString &stored)
{
    const QString text = stored.trimmed();
    if (text.isEmpty())
        return 0;
    if (text.startsWith(QLatin1Char('['))) {
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8());
        return doc.isArray() ? doc.array().size() : 0;
    }
    return 1;
}

}  // namespace

// 构造：记录按值存进 alarm_（之后外部再改这份记录不会影响已打开的框），
// 然后建 UI、读一次盘上的抓拍。全程 GUI 线程，耗时点只有那次图片解码。
AlarmDetailDialog::AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent)
    : QDialog(parent), alarm_(alarm)
{
    setupUi();
    loadScreenshot();
}

// 播放线程必须在这里停：~QDialog 的函数体跑完才轮到对象树析构子控件，
// 反过来（先析构 GL 控件、解码线程还在发帧）就是回调打到已经不存在的窗口上。
AlarmDetailDialog::~AlarmDetailDialog()
{
    // 播放中关框：先停线程（join 之后不会再发 frameReady），再收对象。
    // 顺序反过来就是回调打到正在析构的 GL 控件上。
    stopClipPlayback();
}

// 版面装配。几处不能照字面"简化"的写法：
//   · 尺寸按父窗所在屏取（见下面注释），不用 primaryScreen；
//   · 行序固定为 通道/类别/报警级别/置信度/状态/视频，时间和 uuid 两行已按要求撤下；
//   · 底部三颗钮是手搭的 QHBoxLayout，所以"确认在左、取消在右"这条口径
//     要靠 addWidget 的先后自己保证（Qt 只对标准框按 ButtonRole 重排）。
void AlarmDetailDialog::setupUi()
{
    setWindowTitle("报警详情");
    setStyleSheet(QString("background: %1; color: %2;").arg(theme::PANEL, theme::TEXT));

    // 尺寸按"父窗所在那块屏"取，不按 primaryScreen：调试机上 primary 是外接屏，
    // 而岗位用的 DSI 触摸屏才是这块对话框真正要填满的那块。
    // 480x520 是鼠标时代的值 —— 10 寸 226PPI 屏上约等于 5.4cm x 5.8cm，
    // 里面那张 440x280 的抓拍预览实际只有 2x1.2cm，判不了"这是不是误报"，
    // 而判是不是误报正是这个对话框唯一存在的理由。
    QScreen *scr = parentWidget() ? QGuiApplication::screenAt(parentWidget()->frameGeometry().center())
                                  : nullptr;
    if (!scr) scr = QGuiApplication::primaryScreen();
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
    setMinimumSize(720, 620);
    resize(qMin(1240, avail.width() - 120), qMin(1060, avail.height() - 80));

    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);

    // ── 上半部：抓拍图与片段左右各一格（2026-11-06 指定的版面）─────────────
    // 以前只有图，片段只能去文件管理器里找；现在同一格里"当时那一帧 + 那两秒"
    // 一起看，判误报不用再切窗口。两格等分：图要看得清细节，片段要看得清动作。
    QHBoxLayout *topRow = new QHBoxLayout();
    topRow->setSpacing(12);

    imageLabel_ = new QLabel();
    imageLabel_->setMinimumSize(320, 240);
    imageLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageLabel_->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 4px;")
                                   .arg(theme::BG, theme::BORDER));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setText("无截图");
    topRow->addWidget(imageLabel_, 1);

    setupVideoPane(topRow);
    mainLay->addLayout(topRow);

    // ── 信息区：基础项带标签，施工信息保留常用项 ─────────────────────────────
    // 全量摊开（construction_info 最多近 20 行）会把截图挤没；只留三项又不够判误报。
    // 这里取中间档：基础六项带标签，施工信息保留人/计划/地点/内容/时段，
    // 纯技术键（年月日、旬、顺延、约束）不进详情，仍可在「施工详情」框查看。
    auto addField = [](QGridLayout *grid, int row, int col,
                       const QString &label, const QString &value,
                       const char *color, bool bold) {
        QLabel *lbl = new QLabel(label + QStringLiteral("："));
        lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
        QLabel *val = new QLabel(value);
        val->setStyleSheet(theme::text(color, theme::FS_BODY, bold));
        val->setTextInteractionFlags(Qt::TextSelectableByMouse);
        grid->addWidget(lbl, row, col * 2);
        grid->addWidget(val, row, col * 2 + 1);
        return val;
    };

    // 报警视频片段：硬编环形缓冲区在"报警后 2 秒"才落盘，打开得快时可能还没有。
    QString clipText;
    if (alarm_.videoPath.isEmpty()) {
        clipText = QStringLiteral("未生成");
    } else if (!QFileInfo::exists(alarm_.videoPath)) {
        clipText = QStringLiteral("已清理");
    } else {
        const QFileInfo clip(alarm_.videoPath);
        clipText = QStringLiteral("%1（%2 KB）").arg(clip.fileName()).arg(clip.size() / 1024);
    }

    QString levelText;
    const char *levelColor = theme::TEXT;
    switch (alarm_.alarmLevel) {
    case 1: levelText = QString::fromUtf8("紧急"); levelColor = theme::DANGER; break;
    case 2: levelText = QString::fromUtf8("重要"); levelColor = theme::WARNING; break;
    case 3: levelText = QString::fromUtf8("一般"); break;
    case 4: levelText = QString::fromUtf8("提示"); break;
    default: levelText = QString::fromUtf8("未知（%1）").arg(alarm_.alarmLevel); break;
    }

    QString statusText;
    const char *statusColor = theme::WARNING;   // 未确认
    if (alarm_.status == QLatin1String("rectified")) {
        statusText = QString::fromUtf8("已确认");
        statusColor = theme::SUCCESS;
    } else if (alarm_.status == QLatin1String("disposed")) {
        statusText = QString::fromUtf8("已处置");
        statusColor = theme::INFO;
    } else if (alarm_.status == QLatin1String("false_alarm")) {
        statusText = QString::fromUtf8("误报");
        statusColor = theme::TEXT_MUTED;
    } else {
        statusText = QString::fromUtf8("未确认");
    }

    QGridLayout *infoLay = new QGridLayout();
    infoLay->setHorizontalSpacing(12);
    infoLay->setVerticalSpacing(8);
    addField(infoLay, 0, 0, "通道", QStringLiteral("通道 %1").arg(alarm_.channel + 1),
             theme::TEXT, false);
    addField(infoLay, 0, 1, "类别", alarm_.className, theme::TEXT, false);
    addField(infoLay, 1, 0, "报警级别", levelText, levelColor, true);
    addField(infoLay, 1, 1, "置信度",
             QStringLiteral("%1%").arg(alarm_.confidence * 100, 0, 'f', 1),
             theme::TEXT, false);
    statusLabel_ = addField(infoLay, 2, 0, "状态", statusText, statusColor, true);
    addField(infoLay, 2, 1, "视频", clipText, theme::TEXT, false);

    // 围栏溯源（v1 §13.2.1 的 modelVersion / fenceVersion）：报警产生那刻盖的戳。
    // 现场问"这条是哪版模型、哪一版围栏判出来的"就看这两行，不用再去翻配置页。
    // 空值显示"—"而不是"未知"：这批报警早于打戳功能上线，当时确实没记，
    // 写成"未知版本"看着像程序读挂了，写"—"才是事实。
    // 是不是围栏报警按 alarmType **前缀**判（与 recordFromQuery / 列表那一处同一口径），
    // 不用 AlarmRecord::isFenceAlarm：那个标志是 DAO 回读时才算的，内存里刚产生的那份没有。
    QString fenceVerText;
    if (!alarm_.alarmType.startsWith(QLatin1String("fence"))) {
        fenceVerText = QStringLiteral("—（非围栏报警）");
    } else if (alarm_.fenceVersion > 0) {
        fenceVerText = QStringLiteral("V%1（%2）").arg(alarm_.fenceVersion).arg(alarm_.fenceId);
    } else {
        fenceVerText = QStringLiteral("—");
    }
    addField(infoLay, 3, 0, "模型版本",
             alarm_.modelVersion.isEmpty() ? QStringLiteral("—") : alarm_.modelVersion,
             theme::TEXT_MUTED, false);
    addField(infoLay, 3, 1, "围栏版本", fenceVerText, theme::TEXT_MUTED, false);
    mainLay->addLayout(infoLay);

    // 施工信息：按白名单取常用项，顺序固定；缺失的键直接不显示
    const QVector<session::ConstructionField> construction =
        session::ConstructionInfo::fieldsFromJson(alarm_.constructionJson);
    static const QStringList kKeepFields = {
        QStringLiteral("姓名"), QStringLiteral("负责人（旧）"),
        QStringLiteral("人员工号"), QStringLiteral("负责人工号（旧）"),
        QStringLiteral("负责人电话（旧）"), QStringLiteral("角色"),
        QStringLiteral("施工计划(taskId)"), QStringLiteral("施工id（旧）"),
        QStringLiteral("线路"), QStringLiteral("车站"),
        QStringLiteral("作业内容"), QStringLiteral("施工内容（旧）"),
        QStringLiteral("计划人数"), QStringLiteral("计划时间")};
    QHash<QString, QString> byLabel;
    for (const session::ConstructionField &field : construction)
        byLabel.insert(field.label, field.value);
    QVector<session::ConstructionField> kept;
    for (const QString &label : kKeepFields) {
        if (byLabel.contains(label)) {
            session::ConstructionField field;
            field.label = label;
            field.value = byLabel.value(label);
            kept.append(field);
        }
    }
    if (kept.isEmpty()) {
        QLabel *none = new QLabel(QStringLiteral("施工信息：该报警产生时未登录"));
        none->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        mainLay->addWidget(none);
    } else {
        QLabel *section = new QLabel(QStringLiteral("施工信息"));
        section->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
        mainLay->addWidget(section);
        QGridLayout *infoGrid = new QGridLayout();
        infoGrid->setHorizontalSpacing(10);
        infoGrid->setVerticalSpacing(6);
        int row = 0;
        for (const session::ConstructionField &field : kept) {
            QLabel *lbl = new QLabel(field.label + QStringLiteral("："));
            lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
            QLabel *val = new QLabel(field.value);
            val->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY));
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            infoGrid->addWidget(lbl, row, 0);
            infoGrid->addWidget(val, row, 1);
            ++row;
        }
        mainLay->addLayout(infoGrid);
    }

    // 处置区（2026-11-06 加）：终态行只回显，非终态行才铺可填表单。
    setupDisposeArea(mainLay);

    mainLay->addStretch();
    // 预览区与底部弹簧按 3:1 分剩余高度：图拿到绝大部分，信息网格仍按自身尺寸贴住中间。
    // 不给这一条，Expanding 的预览区只会摊到默认等分，多出来的空间一半沉到按钮上方。
    mainLay->setStretch(0, 3);
    mainLay->setStretch(mainLay->count() - 1, 1);

    // 按钮区域：确认误报 | 不是误报 | 取消
    QHBoxLayout *btnLay = new QHBoxLayout();
    btnLay->setSpacing(12);

    QPushButton *btnFalse = new QPushButton("确认误报");
    btnFalse->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::DANGER)
            .arg(theme::FS_BODY));
    connect(btnFalse, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkFalsePositive);
    btnLay->addWidget(btnFalse);

    QPushButton *btnNormal = new QPushButton("不是误报");
    btnNormal->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::SUCCESS)
            .arg(theme::FS_BODY));
    connect(btnNormal, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkNormal);
    btnLay->addWidget(btnNormal);

    // 「提交处置」只有这一条还不是终态时才铺（disposeForm_ 非空是同一判据，
    // 见 setupDisposeArea）：库里 disposed/false_alarm 的 UPDATE 带终态守卫，
    // 按钮铺出来按下去必然"没反应"，那比不铺更坏。
    if (disposeForm_) {
        QPushButton *btnDispose = new QPushButton("提交处置");
        btnDispose->setStyleSheet(
            QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                    "QPushButton:hover { background: %2; }")
                .arg(theme::TEXT, theme::INFO)
                .arg(theme::FS_BODY));
        connect(btnDispose, &QPushButton::clicked, this, &AlarmDetailDialog::onSubmitDispose);
        btnLay->addWidget(btnDispose);
    }

    btnLay->addStretch();

    QPushButton *btnCancel = new QPushButton("取消");
    btnCancel->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::HOVER)
            .arg(theme::FS_BODY));
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLay->addWidget(btnCancel);

    mainLay->addLayout(btnLay);
}

// 读盘上的抓拍图。两个失败分支都只改文案、不打扰用户：
// 报警图片是按天/按容量清理的，"图没了"是常态而不是错误。
// snapshot_ 存的是原图，缩放交给 rescaleScreenshot（多次 resize 不重复读盘、不逐级劣化）。
//
// 【整幅没了先回退 sidecar】容量闸门只端大文件、保住 64x48 的 *_thumb.jpg
// （见 AlarmManager 的 pruneAlarmMedia），而报警行留 30 天 —— 中间这段窗口里
// "点开是空白"是假的：那天确实拍过。宁可先摆一张糊的一眼，也别让用户以为没取证。
// ⚠ 解码必须走 loadPixmapSafe，**不能**用 snapshot_.load(path) / QPixmap(path)：
//   板端 Qt 的 JPEG 插件与链进来的 libjpeg ABI 冲突，整幅解一张现场抓拍是**进程崩溃**
//   而不是返回失败（本轮实测：拿真 jpg 打开详情框，打完 "Wrong JPEG library version:
//   library is 62, caller expects 80" 就 core dump）。口径见 base/imaging/qt_image_utils.h
//   文件头与 video_alarm_widget.cpp 的缩略图那一处。这个框是岗位每天开几十次的入口，
//   之前一直没崩只是因为媒体按容量清理后大多走到"无截图"那条分支。
void AlarmDetailDialog::loadScreenshot()
{
    if (alarm_.imagePath.isEmpty()) {
        imageLabel_->setText("无截图");
        return;
    }

    QString source = alarm_.imagePath;
    bool thumbnailOnly = false;
    if (!QFile::exists(source)) {
        const QString sidecar = thumbnailSidecarPath(source);
        if (!QFile::exists(sidecar)) {
            imageLabel_->setText("截图已清理");
            return;
        }
        source = sidecar;
        thumbnailOnly = true;
    }

    snapshot_ = loadPixmapSafe(source);
    if (snapshot_.isNull()) {
        imageLabel_->setText("截图加载失败");
        return;
    }
    // 文案挂在 imageLabel_ 上是错的（它是"没图"时的占位文本，有图时不显示），
    // 所以缩略图兜底只在窗口标题里说清，避免"这是原图"的误会。
    setWindowTitle(thumbnailOnly ? QString::fromUtf8("报警详情（截图已被容量清理，显示缩略图）")
                                 : QString::fromUtf8("报警详情"));
    rescaleScreenshot();
}

// ============================================================================
// 片段那一格（2026-11-06 指定：默认不播，点封面才播）
// ============================================================================
// 【为什么默认不播】四路监控的画面本来就在跑，详情框再常驻一路硬解等于在岗位上
// 多开一条解码线程；点开才起流，关框就停（见析构）。
// 【为什么能播的是 .mp4】库里 video_path 存的是 Annex-B 裸流（.h265/.h264），
// 那是硬编环形缓冲直接落盘的产物，没有容器也没有时长；同名 .mp4 是事后封的一份，
// 只有它能正常播放。判据与 pad_wire 的 clipUrlOf() 同一句。
// 【封面为什么用抓拍图】片段的第一帧要解码才有，为一个封面再起一路流是本末倒置；
// 抓拍与片段是同一时刻，认得出就是那一段。
// ============================================================================
QString AlarmDetailDialog::playableClipPath() const
{
    if (alarm_.videoPath.isEmpty())
        return QString();
    const QFileInfo clip(alarm_.videoPath);
    const QString mp4 = clip.absolutePath() + QLatin1Char('/')
                        + clip.completeBaseName() + QStringLiteral(".mp4");
    return QFile::exists(mp4) ? mp4 : QString();
}

void AlarmDetailDialog::setupVideoPane(QHBoxLayout *topRow)
{
    videoPane_ = new QWidget(this);
    videoStack_ = new QStackedLayout(videoPane_);
    videoStack_->setContentsMargins(0, 0, 0, 0);

    videoPoster_ = new QWidget(videoPane_);
    videoPoster_->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 4px;")
                                    .arg(theme::BG, theme::BORDER));
    QVBoxLayout *posterLay = new QVBoxLayout(videoPoster_);
    posterLay->setContentsMargins(1, 1, 1, 1);
    posterLay->setSpacing(4);

    clipPosterImage_ = new QLabel(videoPoster_);
    clipPosterImage_->setMinimumSize(320, 240);
    clipPosterImage_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    clipPosterImage_->setAlignment(Qt::AlignCenter);
    posterLay->addWidget(clipPosterImage_, 1);

    clipHint_ = new QLabel(videoPoster_);
    clipHint_->setAlignment(Qt::AlignCenter);
    clipHint_->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    posterLay->addWidget(clipHint_);

    clipView_ = new GLVideoWidget(videoPane_);
    clipView_->setMinimumSize(320, 240);

    videoStack_->addWidget(videoPoster_);   // 0：封面（默认）
    videoStack_->addWidget(clipView_);      // 1：真在放
    videoStack_->setCurrentIndex(0);
    topRow->addWidget(videoPane_, 1);

    if (playableClipPath().isEmpty()) {
        // 没得播就别装成"点我"：光标与文案都要说清是没生成还是已被清理——
        // 这两种在现场是不同的事（前者是还没写完，后者是媒体按容量清掉了）。
        clipHint_->setText(alarm_.videoPath.isEmpty()
                               ? QString::fromUtf8("视频未生成（报警后约 2 秒才写）")
                               : QString::fromUtf8("视频已清理"));
    } else {
        clipHint_->setText(QString::fromUtf8("▶ 点击播放片段"));
        videoPoster_->setCursor(Qt::PointingHandCursor);
        videoPoster_->installEventFilter(this);
    }
    rescaleScreenshot();
}

void AlarmDetailDialog::startClipPlayback()
{
    const QString mp4 = playableClipPath();
    if (mp4.isEmpty())
        return;

    if (!clipDecoder_) {
        // PreviewOnly：不建 PpeTask、不分配 640 缓冲池、不碰 NPU（见该枚举的注释）。
        // **parent 必须是 nullptr**：FFmpegVideoDecoder::start() 里要 moveToThread(thread_)
        // 把自己搬到解码线程，而带 parent 的对象 Qt 拒绝移动（实测打
        // "QObject::moveToThread: Cannot move objects with a parent"）——移动失败就是
        // decodeLoop 挂在 GUI 线程上跑，点一下封面整个界面卡住到片段放完。
        // 与 qr_scan_dialog / 智能识别页同一口径：无 parent，收尾自己 stop() + delete。
        clipDecoder_ = new FFmpegVideoDecoder(nullptr,
                                              FFmpegVideoDecoder::InferenceMode::PreviewOnly);
        connect(clipDecoder_, &FFmpegVideoDecoder::frameReady,
                clipView_, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
        connect(clipDecoder_, &FFmpegVideoDecoder::error, this, [this](const QString &msg) {
            qWarning().noquote() << "[AlarmDetail] 片段播放失败:" << msg;
            // 这里**不能**调 stopClipPlayback()（它会 delete 发信号的那个对象）：
            // start() 里"上一次解码线程还没退干净"那条拒绝分支是**在 GUI 线程同步**
            // emit error 的，本槽于是直连、在 emit 的调用栈里把 sender 删掉，
            // start() 返回时继续摸自己的成员就是 use-after-free。
            // 退回封面让用户再点一次即可，真正的收尾交给析构或下一次点击。
            videoStack_->setCurrentIndex(0);
            clipHint_->setText(QString::fromUtf8("▶ 播放失败：%1").arg(msg));
        });
        // 文件源放完就是 PassEnd::StreamEnded（没开 loop，也不会重连），finished 是唯一
        // 的"放完了"通知。回到封面而不是停在最后一帧：冻住的那帧容易被当成还在播。
        connect(clipDecoder_, &FFmpegVideoDecoder::finished, this, [this]() {
            if (videoStack_->currentIndex() == 1) {
                videoStack_->setCurrentIndex(0);
                clipHint_->setText(QString::fromUtf8("▶ 再点一次可重播"));
            }
        });
    }
    videoStack_->setCurrentIndex(1);
    clipDecoder_->start(mp4);
}

void AlarmDetailDialog::stopClipPlayback()
{
    if (clipDecoder_) {
        // stop() 已经把解码线程 join 掉（frameReady 不可能再发），所以这里可以直接 delete；
        // 无 parent 的对象放到 deleteLater 反而要等事件循环，关框路径不一定还转。
        clipDecoder_->stop();
        delete clipDecoder_;
        clipDecoder_ = nullptr;
    }
    if (videoStack_)
        videoStack_->setCurrentIndex(0);
}

bool AlarmDetailDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == videoPoster_ && event->type() == QEvent::MouseButtonRelease) {
        startClipPlayback();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}

// ============================================================================
// rescaleScreenshot / resizeEvent - 预览图按当前预览区重画
// ============================================================================
// 只在构造时缩一次的话，图就被钉在 label 的**最小**尺寸上（对话框后来长到 1240x1060，
// 图还是那张 660x420）。原始 pixmap 存在 snapshot_ 里，缩放不重复读盘、也不会
// 在多次 resize 之间逐次劣化画质。
// ============================================================================
void AlarmDetailDialog::rescaleScreenshot()
{
    // 减 2 是那条 1px 边框：不减的话 KeepAspectRatio 算出来的图永远比可视区宽 2px，
    // label 会开始考虑滚动条，预览区右下一条边被吃掉。
    if (!snapshot_.isNull()) {
        if (imageLabel_) {
            const QSize box = imageLabel_->size() - QSize(2, 2);
            imageLabel_->setPixmap(snapshot_.scaled(box, Qt::KeepAspectRatio,
                                                    Qt::SmoothTransformation));
        }
        // 封面用的是同一张抓拍（片段第一帧要解码才有，为一个封面再起一路不值当），
        // 所以缩放判据与上面完全一致，只是目标格子换成视频那一格。
        if (clipPosterImage_) {
            const QSize box = clipPosterImage_->size() - QSize(2, 2);
            clipPosterImage_->setPixmap(snapshot_.scaled(box, Qt::KeepAspectRatio,
                                                         Qt::SmoothTransformation));
        }
    }
}

// 转发给 rescaleScreenshot——本框的尺寸来源有三条（构造时的 resize、全屏/窗口切换、
// 用户拖拽），只有接事件这一条路能让图跟得上。
void AlarmDetailDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    rescaleScreenshot();
}

// 「确认误报」：唯一的二次确认。三点注意：
//   1) parent 传 nullptr + WindowStaysOnTopHint：确认框要压在本详情框之上，
//      否则岗位上点不到（两层模态的老坑，见 construction_detail_dialog.h 文件头）；
//   2) localizeMessageBoxButtons：板上的 qt_zh_CN.qm 没译 QPlatformTheme 上下文，
//      标准按钮默认是英文 Yes/No/Cancel。全局 QEvent::Show 过滤器本来也会兜这一层
//      （qthelper 的 installMessageBoxTextFilter），这里显式再调一次是"在 show 之前
//      就把文案定稿"，adjustSize() 才能量到中文宽度，也让读代码的人不用去找过滤器。
//   3) 居中按"本框所在屏"算，不是 primaryScreen——调试机上 primary 是外接屏。
void AlarmDetailDialog::onMarkFalsePositive()
{
    QMessageBox box(QMessageBox::Question, tr("确认误报"), tr("确定将此报警标记为误报？\n标记后状态将变为误报。"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QtHelper::localizeMessageBoxButtons(&box);
    box.adjustSize();

    // 按"本对话框所在屏"居中，不是 primaryScreen：双屏调试机上 primary 是外接屏，
    // 这个确认框会整块弹到没有触摸的那台显示器上，岗位上按不到。与 frmMain::systemExit 同一条口径。
    if (QScreen *screen = QGuiApplication::screenAt(geometry().center())) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();

    if (box.exec() == QMessageBox::Ok) {
        emit alarmMarkedFalsePositive(alarm_.id);
        accept();
    }
}

void AlarmDetailDialog::onMarkNormal()
{
    emit alarmAcknowledged(alarm_.id);
    accept();
}

// ============================================================================
// 处置区（2026-11-06 加）
// ============================================================================
// 【为什么表单在框里、写库在外面】本框一直的信条是"只读不改"（见头文件），
// 状态变化都 emit 给调用方。处置多了两个字段（选项 + 意见）和一批照片文件，
// 但改库仍然只有 AlarmManager::disposeAlarmById() 一处（它同时动内存窗口那份副本，
// 少动一处就会被关机时的整批回写打回 pending）。
// 【选项表】读 alarm_labels::disposeOptions()，与 Pad 的 GET /api/alarms/{id}/dispose-options
// 同一份，所以两端看到的四条措辞永远一致；库里另存一列 dispose_option_id，
// 以后改措辞不至于让"已现场纠正"的历史统计接不上。
// 【照片只能拍】CaptureDialog 里没有相册入口，这是 2026-11-06 现场明确指定的口径：
// 整改照片要证明人到过现场，从相册挑一张旧图等于伪造证据。
// 【终态不回退】status 已是 disposed/false_alarm 的行只回显，不铺表单：库里那条
// UPDATE 带终态守卫（AlarmDAO::dispose），按钮铺出来按下去必然"没反应"，比不铺更坏。
// ============================================================================
void AlarmDetailDialog::setupDisposeArea(QVBoxLayout *mainLay)
{
    const bool terminal = alarm_.status == QLatin1String("disposed")
                          || alarm_.status == QLatin1String("false_alarm");

    auto sectionLabel = [this](const QString &text) {
        QLabel *lbl = new QLabel(text, this);
        lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
        return lbl;
    };

    if (terminal) {
        // 回显：库里已经有的那几项。照片只报张数——缩略图条留给后续版本，
        // 现在把"谁在什么时候按哪条措施处置的"说清楚就够判了。
        mainLay->addWidget(sectionLabel(QString::fromUtf8("处置记录")));
        QGridLayout *grid = new QGridLayout();
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(6);
        int row = 0;
        auto addRow = [&grid, &row, this](const QString &label, const QString &value) {
            QLabel *lbl = new QLabel(label + QStringLiteral("："), this);
            lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
            QLabel *val = new QLabel(value, this);
            val->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY));
            val->setTextInteractionFlags(Qt::TextSelectableByMouse);
            grid->addWidget(lbl, row, 0);
            grid->addWidget(val, row, 1);
            ++row;
        };
        const int photoCount = disposePhotoCountFromJson(alarm_.disposePhoto);
        addRow(QString::fromUtf8("处置措施"),
               session::ConstructionInfo::display(alarm_.disposeResult));
        addRow(QString::fromUtf8("整改意见"), session::ConstructionInfo::display(alarm_.remark));
        addRow(QString::fromUtf8("处置人"),
               session::ConstructionInfo::display(alarm_.disposeUserName));
        addRow(QString::fromUtf8("处置时间"),
               session::ConstructionInfo::display(alarm_.disposeTime));
        addRow(QString::fromUtf8("处置照片"),
               photoCount > 0 ? QStringLiteral("%1 张").arg(photoCount)
                              : QString::fromUtf8("无"));
        mainLay->addLayout(grid);
        return;
    }

    disposeForm_ = new QWidget(this);
    QVBoxLayout *form = new QVBoxLayout(disposeForm_);
    form->setContentsMargins(0, 0, 0, 0);
    form->setSpacing(8);

    form->addWidget(sectionLabel(QString::fromUtf8("处置（单选，必填）")));

    // 四条横排：10 寸屏一行放得下（每条 5~6 字 + 圆点），竖排会把预览区挤小。
    QHBoxLayout *options = new QHBoxLayout();
    options->setSpacing(14);
    disposeGroup_ = new QButtonGroup(disposeForm_);
    disposeGroup_->setExclusive(true);
    for (const alarm_labels::DisposeOption &one : alarm_labels::disposeOptions()) {
        QRadioButton *rb = new QRadioButton(one.label, disposeForm_);
        rb->setStyleSheet(QString("QRadioButton { color: %1; font-size:%2px; }"
                                  "QRadioButton::indicator { width:20px; height:20px; }")
                              .arg(theme::TEXT)
                              .arg(theme::FS_BODY));
        rb->setProperty("optionId", one.id);
        disposeGroup_->addButton(rb, one.id);
        options->addWidget(rb);
    }
    options->addStretch();
    form->addLayout(options);

    QHBoxLayout *remarkLay = new QHBoxLayout();
    remarkLay->setSpacing(10);
    QLabel *remarkLbl = new QLabel(QString::fromUtf8("整改意见"), disposeForm_);
    remarkLbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    remarkLay->addWidget(remarkLbl);
    disposeRemark_ = new QLineEdit(disposeForm_);
    disposeRemark_->setPlaceholderText(QString::fromUtf8("可留空，最多 200 字"));
    disposeRemark_->setMaxLength(200);
    disposeRemark_->setStyleSheet(
        QString("QLineEdit { color: %1; background: %2; border: 1px solid %3;"
                " border-radius: 4px; padding: 6px 10px; font-size:%4px; }")
            .arg(theme::TEXT, theme::FIELD, theme::BORDER)
            .arg(theme::FS_BODY));
    remarkLay->addWidget(disposeRemark_, 1);
    form->addLayout(remarkLay);

    QHBoxLayout *photoLay = new QHBoxLayout();
    photoLay->setSpacing(10);
    snapButton_ = new QPushButton(QString::fromUtf8("拍照"), disposeForm_);
    snapButton_->setMinimumSize(96, 40);
    snapButton_->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px;"
                " padding: 8px 18px; font-size:%3px; }QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::BTN_SECONDARY)
            .arg(theme::FS_BODY));
    connect(snapButton_, &QPushButton::clicked, this, &AlarmDetailDialog::onTakeDisposePhoto);
    photoLay->addWidget(snapButton_);

    auto *clearButton = new QPushButton(QString::fromUtf8("清空照片"), disposeForm_);
    clearButton->setMinimumSize(96, 40);
    clearButton->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px;"
                " padding: 8px 18px; font-size:%3px; }QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::HOVER)
            .arg(theme::FS_BODY));
    // 清掉的是"本框现拍、还没提交"的那些，所以删文件是安全的：库里这条还是 pending。
    connect(clearButton, &QPushButton::clicked, this, [this]() {
        for (const QString &abs : disposePhotos_)
            QFile::remove(abs);
        disposePhotos_.clear();
        photoCountLabel_->setText(QString::fromUtf8("处置照片：0/%1 张（只能现场拍照）")
                                      .arg(kMaxDisposePhotos));
    });
    photoLay->addWidget(clearButton);

    photoCountLabel_ = new QLabel(
        QString::fromUtf8("处置照片：0/%1 张（只能现场拍照）").arg(kMaxDisposePhotos),
        disposeForm_);
    photoCountLabel_->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    photoLay->addWidget(photoCountLabel_);
    photoLay->addStretch();
    form->addLayout(photoLay);

    mainLay->addWidget(disposeForm_);
}

void AlarmDetailDialog::onTakeDisposePhoto()
{
    if (disposePhotos_.size() >= kMaxDisposePhotos)
        return;
    if (!isSafeAlarmIdForPath(alarm_.id)) {
        QMessageBox::warning(this, QString::fromUtf8("处置"),
                             QString::fromUtf8("这条报警的 id 不适合建照片目录，已拒绝拍照。"));
        return;
    }

    // 目录完全由本机定：<媒体根>/dispose/<alarmId>/，与 POST /api/alarms/{id}/dispose
    // 那一套同名同序（dispose1.jpg…）。alarmId 是 UUID，不含路径分隔符，所以拼进去安全；
    // Pad 那边要校验 [A-Za-z0-9_-] 是因为 id 由对端带上来，这里不是。
    const QString dir = QDir(mediaRootAbs()).filePath(
        QStringLiteral("dispose/%1").arg(alarm_.id));

    CaptureDialog dlg(dir, kMaxDisposePhotos - disposePhotos_.size(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;   // 拍照框自己会把没被采用的那张删掉，这里不用收尾

    for (const QString &abs : dlg.photoPaths()) {
        if (disposePhotos_.contains(abs))
            continue;
        disposePhotos_ << abs;
        disposePhotosTaken_ << abs;
    }
    photoCountLabel_->setText(
        QString::fromUtf8("处置照片：%1/%2 张（只能现场拍照）")
            .arg(disposePhotos_.size()).arg(kMaxDisposePhotos));
}

void AlarmDetailDialog::onSubmitDispose()
{
    QAbstractButton *chosen = disposeGroup_ ? disposeGroup_->checkedButton() : nullptr;
    if (!chosen) {
        QMessageBox::warning(this, QString::fromUtf8("处置"),
                             QString::fromUtf8("先选一条处置措施。"));
        return;
    }

    // 单张超 5MB 的按 §4.2 拒收。这里**不放行提交**：剔掉就交上去的话，界面上一句
    // "请重拍"和库里一条"已处置"会同时存在，岗位分不清自己到底处置过没有。
    QJsonArray stored;
    QStringList oversized;
    for (const QString &abs : disposePhotos_) {
        const QFileInfo info(abs);
        if (!info.exists())
            continue;                       // 已经被删掉的（拍照框里点过"删除"）不算
        if (info.size() > kMaxDisposePhotoBytes) {
            oversized << info.fileName();
            continue;
        }
        // 库里存**相对媒体根**的路径，出 URL 时才拼前缀（与 pad 那条写入口一致）
        stored.append(QDir(mediaRootAbs()).relativeFilePath(abs));
    }
    if (!oversized.isEmpty()) {
        QMessageBox::warning(
            this, QString::fromUtf8("处置"),
            QString::fromUtf8("有 %1 张超过 5MB（%2），请重拍后再提交。")
                .arg(oversized.size()).arg(oversized.join(QStringLiteral("、"))));
        return;
    }

    const QString measure = chosen->text();
    const QString remark = disposeRemark_ ? disposeRemark_->text().trimmed() : QString();
    const QString photosJson =
        QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact));

    // 先落库、写成了才 accept()：顺序反过来就是"框关了、照片留在盘上、库里那条还是
    // pending"——那是比报错更难发现的状态。失败时表单原样留着（照片还在列表里），
    // 用户改一下就能重试。
    QString err;
    if (!ui::commitAlarmDisposal(alarm_.id, measure, remark,
                                 chosen->property("optionId").toInt(), photosJson, &err)) {
        QMessageBox::warning(this, QString::fromUtf8("处置没写进去"), err);
        return;
    }

    // 亮"已提交"再关框：accept() 之后会走 reject/closeEvent 那两条收口路径，
    // 没有这个标记就会把刚交出去的照片从盘上删掉。
    submitted_ = true;
    emit alarmDisposed(alarm_.id);
    accept();
}

void AlarmDetailDialog::discardUnsubmittedPhotos()
{
    if (submitted_)
        return;
    for (const QString &abs : disposePhotosTaken_) {
        if (!disposePhotos_.contains(abs))
            QFile::remove(abs);   // 拍照框里被"删除"掉的那张它也自己删了，这里只兜漏
    }
    for (const QString &abs : disposePhotos_)
        QFile::remove(abs);
    disposePhotos_.clear();
    disposePhotosTaken_.clear();
}

void AlarmDetailDialog::reject()
{
    discardUnsubmittedPhotos();
    QDialog::reject();
}

void AlarmDetailDialog::closeEvent(QCloseEvent *event)
{
    discardUnsubmittedPhotos();
    QDialog::closeEvent(event);
}

// ============================================================================
// ui::commitAlarmDisposal - 「提交处置」的落库收口
// ============================================================================
// 处置人取当前扫码登录会话：现场是"谁登录谁处置"，工号/姓名与报警行上那份施工信息
// 同源。没登录时两列都空 —— 宁可空着也不填一个"操作员"上去，那是假数。
namespace ui {

bool commitAlarmDisposal(const QString &alarmId, const QString &measure, const QString &remark,
                         int optionId, const QString &photoPathsJson, QString *err)
{
    const session::ConstructionInfo person = session::ConstructionSession::instance().snapshot();
    QString reason;
    // disposedAtInternal 传空串 = 由 DAO 用本机此刻（点提交就是"现在做完的"）。
    // 走 disposeAlarmById 而不是 AlarmDAO::dispose()：它同时动内存窗口里那份同 id
    // 副本，少了那一步，关机时 syncToDatabase() 的整批 INSERT OR REPLACE 会把这条
    // 处置连带照片路径一起打回 pending（alarm_manager.h 上那段注释记的就是这个坑）。
    const bool ok = AlarmManager::instance().disposeAlarmById(
        alarmId, measure, remark, person.personId, person.personName,
        photoPathsJson, QString(), optionId, &reason);
    if (!ok && err)
        *err = reason.isEmpty() ? QString::fromUtf8("库里没写成，请重试") : reason;
    return ok;
}

}  // namespace ui
