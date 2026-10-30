#pragma once

// ============================================================================
// capture_button.h - 圆形快门按钮（拍照类对话框共用）
// ============================================================================
//
// 原来定义在智能识别页的匿名 namespace 里，二维码与 OCR 两处共用。
// 现在二维码流程抽成 QrScanDialog（src/ui/widgets/），这个按钮被两边都要用，
// 留一份定义在这里，避免两个快门按钮长得不一样这种肉眼才看得出的分叉。
// 2026-10-30 二维码改回打开即实时扫描、不再用快门，当前唯一使用者是 OCR 拍照流程；
// 定义仍留在这里（智能识别页的 .cpp 带 moc，控件定义越少越好），
// 再出现一个拍照对话框时不必又画第二个快门。
//
// 画法：白底外环 + 深色内环，88x88 定尺寸（触屏最小命中区口径，见 src/ui/core/README.md）。
// 无信号无槽，纯 paintEvent，所以头文件内联即可，不需要 .cpp 与 Q_OBJECT。
//
// ============================================================================

#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include "theme.h"

namespace uiwidgets {

class CaptureButton : public QPushButton {
public:
    explicit CaptureButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setFixedSize(88, 88);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = rect().adjusted(2, 2, -2, -2);
        painter.setPen(QPen(QColor(255, 255, 255, 190), 5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(outer);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(theme::BG));
        painter.drawEllipse(outer.adjusted(20, 20, -20, -20));
    }
};

} // namespace uiwidgets
