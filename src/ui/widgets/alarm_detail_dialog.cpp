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
// 【只读不改】状态变化一律 emit 信号给调用方，本文件里没有任何写库/写单例的调用。
// ============================================================================
#include "alarm_detail_dialog.h"
#include "construction_session.h"
#include "qthelper.h"
#include "theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QPixmap>
#include <QScreen>
#include <QGuiApplication>
#include <QVector>

// 构造：记录按值存进 alarm_（之后外部再改这份记录不会影响已打开的框），
// 然后建 UI、读一次盘上的抓拍。全程 GUI 线程，耗时点只有那次图片解码。
AlarmDetailDialog::AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent)
    : QDialog(parent), alarm_(alarm)
{
    setupUi();
    loadScreenshot();
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

    // 截图预览区域（拿走样条的主要份额，见下方 setStretch）
    imageLabel_ = new QLabel();
    imageLabel_->setMinimumSize(660, 420);
    imageLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    imageLabel_->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 4px;")
                                   .arg(theme::BG, theme::BORDER));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setText("无截图");
    mainLay->addWidget(imageLabel_);

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
void AlarmDetailDialog::loadScreenshot()
{
    if (alarm_.imagePath.isEmpty() || !QFile::exists(alarm_.imagePath)) {
        imageLabel_->setText("无截图");
        return;
    }

    snapshot_.load(alarm_.imagePath);
    if (snapshot_.isNull()) {
        imageLabel_->setText("截图加载失败");
        return;
    }
    rescaleScreenshot();
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
    if (snapshot_.isNull() || !imageLabel_) return;
    // 减 2 是那条 1px 边框：不减的话 KeepAspectRatio 算出来的图永远比可视区宽 2px，
    // label 会开始考虑滚动条，预览区右下一条边被吃掉。
    const QSize box = imageLabel_->size() - QSize(2, 2);
    imageLabel_->setPixmap(snapshot_.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
