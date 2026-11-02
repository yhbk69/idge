// ============================================================================
// theme.cpp - 设计令牌构造器实现（令牌定义见 theme.h）
// ============================================================================

#include "theme.h"
#include <QString>
#include <QComboBox>
#include <QEvent>
#include <QStyledItemDelegate>

namespace
{

// 只改高度，其余全部交给 QStyledItemDelegate（字体、对齐、图标位置都不动）。
class TouchItemDelegate : public QStyledItemDelegate
{
public:
    TouchItemDelegate(int rowH, QObject *parent)
        : QStyledItemDelegate(parent), rowH_(rowH) {}

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize s = QStyledItemDelegate::sizeHint(option, index);
        s.setHeight(qMax(s.height(), rowH_));
        return s;
    }

private:
    int rowH_;
};

// 下拉不吃滚轮。滚动区里一次"想翻页"的滚轮手势会落在下拉上，当场改掉当前项并触发
// activated —— 而 activated 的槽是"写配置 + 落盘"，于是配置被一次无意的滚动改了
// （2026-10-31 板上实测：把某模型的「跟随全局」滚成了 0.15 并写进了 config.json）。
class WheelGuard : public QObject
{
public:
    explicit WheelGuard(QObject *parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject *, QEvent *e) override
    {
        if (e->type() == QEvent::Wheel) {
            e->ignore();
            return true;
        }
        return false;
    }
};

}  // namespace

namespace theme
{

QString text(const char *color, int px, bool bold)
{
    return QString("color:%1;font-size:%2px;%3")
        .arg(color)
        .arg(px)
        .arg(bold ? QStringLiteral("font-weight:bold;") : QString());
}

QString chip(const char *fg, const char *bg, int px, int radius)
{
    // 徽标只把 padding 从 2x8 抬到 3x9，**不设 min-height**：
    // chip 常贴在按钮/卡片内部，撑高会把宿主顶变形。
    return QString("color:%1;background:%2;font-size:%3px;border-radius:%4px;padding:3px 9px;")
        .arg(fg, bg)
        .arg(px)
        .arg(radius);
}

QString field(const char *bg)
{
    // T2：min-height 不含 padding/border → 总高 42 + 2*6 + 2*1 = 56px ≈ 6.3mm。
    // 不写这一句，blacksoft.css 的输入件档位会被每一处内联 field() 打穿。
    return QString("color:%1;background:%2;border:1px solid %3;border-radius:4px;"
                   "min-height:%4px;padding:6px 10px;")
        .arg(TEXT, bg, BORDER)
        .arg(TOUCH_FIELD_MIN_H);
}

QString miniButton(const char *bg)
{
    // T3：30 + 2*8 + 2*1 = 48px ≈ 5.4mm（原来是 3px 上下 padding ≈ 24px，只有 2.7mm）。
    return QString("color:%1;background:%2;border-radius:4px;"
                   "min-height:%3px;padding:8px 14px;")
        .arg(TEXT, bg)
        .arg(TOUCH_ROW_MIN_H);
}

QString mainButton(const char *bg)
{
    // T1：54 + 2*8 + 2*1 = 72px ≈ 8.1mm。padding 与 miniButton 同值，只有 min-height 差档，
    // 这样同一批按钮换档时横向留白一致，不会出现"高度变了宽度没变"的错位。
    return QString("color:%1;background:%2;border-radius:4px;"
                   "min-height:%3px;padding:8px 14px;")
        .arg(TEXT, bg)
        .arg(TOUCH_MAIN_MIN_H);
}

void makeComboTouch(QComboBox *combo, int rowH)
{
    if (!combo)
        return;
    combo->setItemDelegate(new TouchItemDelegate(rowH, combo));
    combo->installEventFilter(new WheelGuard(combo));
}

}  // namespace theme
