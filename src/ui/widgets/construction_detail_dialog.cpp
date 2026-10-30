// ============================================================================
// construction_detail_dialog.cpp - 「当前登录的施工信息」详情框实现
// ============================================================================

#include "construction_detail_dialog.h"

#include "construction_session.h"
#include "theme.h"

#include <QGridLayout>
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

    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"));
    closeBtn->setStyleSheet(QString("QPushButton { color:%1; background:%2; border:none; border-radius:6px; "
                                    "min-height:%3px; padding:8px 28px; font-size:%4px; }"
                                    "QPushButton:hover { background:%5; }")
                                .arg(theme::TEXT, theme::BTN_SECONDARY)
                                .arg(theme::TOUCH_MAIN_MIN_H)
                                .arg(theme::FS_CARD)
                                .arg(theme::BTN_SECONDARY_HOVER));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    mainLay->addWidget(closeBtn, 0, Qt::AlignRight);
}
