// ============================================================================
// construction_detail_dialog.cpp - 「当前登录的施工信息」详情框实现
// ============================================================================

#include "construction_detail_dialog.h"

#include "construction_session.h"
#include "theme.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVector>

namespace {

QLabel *makeValueLabel(const QString &text)
{
    QLabel *val = new QLabel(text);
    val->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY, true));
    // 现场要照着抄电话号码，值必须能选中（触屏上的长按选中走的就是鼠标事件）
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    val->setWordWrap(true);
    return val;
}

} // namespace

ConstructionDetailDialog::ConstructionDetailDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
}

void ConstructionDetailDialog::setupUi()
{
    const session::ConstructionInfo info = session::ConstructionSession::instance().snapshot();

    setWindowTitle(QString::fromUtf8("施工详情"));
    setMinimumSize(520, 420);
    setStyleSheet(QString("QDialog { background:%1; }").arg(theme::PANEL));

    auto *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(24, 24, 24, 24);
    mainLay->setSpacing(16);

    auto *title = new QLabel(QString::fromUtf8("当前登录的施工信息"));
    title->setStyleSheet(theme::text(theme::ACCENT, theme::FS_PAGE, true));
    mainLay->addWidget(title);

    // 逐键渲染：fields() 里有什么就列什么，加字段不改本文件（口径见 construction_session.h）
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);
    int row = 0;
    const QVector<session::ConstructionField> fields = session::ConstructionInfo::fields(info.toJson());
    for (const session::ConstructionField &f : fields) {
        auto *label = new QLabel(f.label + QStringLiteral("："));
        label->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        grid->addWidget(label, row, 0);
        grid->addWidget(makeValueLabel(f.value), row, 1);
        ++row;
    }
    mainLay->addLayout(grid);

    auto *note = new QLabel(QString::fromUtf8(
        "本时段产生的每一条报警都会记到这份信息名下（落库、报警详情、WebSocket 上行同一份）。"));
    note->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    note->setWordWrap(true);
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLay->addWidget(note);

    mainLay->addStretch();

    // 底部两颗钮分置两端、中间留弹：退出登录是**有后果**的动作（停四路检测、
    // 合回登录门），跟"关闭"挨着放就会被一次顺手的误触替掉。
    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(12);

    auto *logoutBtn = new QPushButton(QString::fromUtf8("退出登录"));
    logoutBtn->setStyleSheet(QString("QPushButton { color:%1; background:%2; border:none; border-radius:6px;"
                                     "min-height:%3px; padding:8px 28px; font-size:%4px; }"
                                     "QPushButton:hover { background:%5; }")
                                 .arg("#FFFFFF", theme::DANGER)
                                 .arg(theme::TOUCH_MAIN_MIN_H)
                                 .arg(theme::FS_CARD)
                                 .arg(theme::DANGER_PRESSED));
    // 这里只记下"用户要退"并把框关掉；真正的登出由调用方在 exec() 返回后走
    // frmMain::onLogoutRequested()。理由见头文件：那一步还要弹自己的确认框，
    // 套在本框的模态循环里就是两层模态，确认框压在详情框下面按不到。
    connect(logoutBtn, &QPushButton::clicked, this, [this]() {
        logoutRequested_ = true;
        reject();
    });
    btnRow->addWidget(logoutBtn);
    btnRow->addStretch();

    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"));
    closeBtn->setStyleSheet(QString("QPushButton { color:%1; background:%2; border:none; border-radius:6px; "
                                    "min-height:%3px; padding:8px 28px; font-size:%4px; }"
                                    "QPushButton:hover { background:%5; }")
                                .arg(theme::TEXT, theme::BTN_SECONDARY)
                                .arg(theme::TOUCH_MAIN_MIN_H)
                                .arg(theme::FS_CARD)
                                .arg(theme::BTN_SECONDARY_HOVER));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnRow->addWidget(closeBtn);

    mainLay->addLayout(btnRow);
}
