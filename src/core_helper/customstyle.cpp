#include "customstyle.h"
#include "qapplication.h"
#include "qpalette.h"

// ============================================================================
// initStyle - 初始化全局样式
// 通过Qt样式表(QSS)设置应用程序的全局外观
// QSS语法类似于CSS，用于定义Qt控件的视觉样式
// ============================================================================
void CustomStyle::initStyle(int fontSize, int radioButtonSize, int checkBoxSize, int sliderHeight)
{
    if (fontSize <= 0) {
        return;
    }

    QStringList list;

    // ============================================================================
    // 全局字体样式
    // *{font-size:XXpx;} 应用到所有控件
    // ============================================================================
    list << QString("*{font-size:%1px;}").arg(fontSize);

    // ============================================================================
    // 单选框指示器尺寸
    // QRadioButton::indicator 选择器控制单选框的显示大小
    // ============================================================================
    list << QString("QRadioButton::indicator{width:%1px;height:%1px;}").arg(radioButtonSize);

    // ============================================================================
    // 复选框指示器尺寸
    // 同时设置QCheckBox、QGroupBox、QTreeWidget、QListWidget的指示器大小
    // ============================================================================
    list << QString("QCheckBox::indicator,QGroupBox::indicator,QTreeWidget::indicator,QListWidget::indicator{width:%1px;height:%1px;}").arg(checkBoxSize);

    // ============================================================================
    // 滑块样式配置
    // 从系统调色板获取颜色，确保与系统主题一致
    // ============================================================================
#if 0
    // 硬编码颜色方案（备用）
    QString normalColor = "#e3e3e3";
    QString grooveColor = "#0078d7";
    QString handleColor = "#FFFFFF";
    QString borderColor = "#9B9B9B";
#else
    // 从系统调色板获取颜色（自适应系统主题）
    QPalette palette;
    for (int i = 0; i < 21; ++i) {
        //qDebug() << i << palette.color((QPalette::ColorRole)i).name();
    }

    QString normalColor = palette.color(QPalette::Midlight).name();    // 滑块轨道背景色
    QString grooveColor = palette.color(QPalette::Highlight).name();   // 滑块已选区域色
    QString handleColor = palette.color(QPalette::Light).name();       // 滑块手柄颜色
    QString borderColor = palette.color(QPalette::Shadow).name();      // 滑块手柄边框色
#endif

    // 计算滑块几何参数
    int sliderRadius = sliderHeight / 2;                     // 轨道圆角半径
    int handleWidth = (sliderHeight * 3) / 2 + (sliderHeight / 5);  // 手柄宽度
    int handleRadius = handleWidth / 2 + 1;                 // 手柄圆角半径
    int handleOffset = handleRadius / 2;                     // 手柄偏移量（用于居中）

    // ============================================================================
    // 横向滑块样式
    // ::horizontal 选择器仅应用于水平方向滑块
    // ============================================================================
    list << QString("QSlider::horizontal{min-height:%1px;}").arg(sliderHeight * 2);
    // 轨道（groove）：滑块的背景条
    list << QString("QSlider::groove:horizontal{background:%1;height:%2px;border-radius:%3px;}")
         .arg(normalColor).arg(sliderHeight).arg(sliderRadius);
    // 已选区域（add-page）：滑块右侧区域
    list << QString("QSlider::add-page:horizontal{background:%1;height:%2px;border-radius:%3px;}")
         .arg(normalColor).arg(sliderHeight).arg(sliderRadius);
    // 未选区域（sub-page）：滑块左侧区域
    list << QString("QSlider::sub-page:horizontal{background:%1;height:%2px;border-radius:%3px;}")
         .arg(grooveColor).arg(sliderHeight).arg(sliderRadius);
    // 手柄（handle）：可拖动的滑块按钮
    list << QString("QSlider::handle:horizontal{border:1px solid %5;width:%2px;margin-top:-%3px;margin-bottom:-%3px;border-radius:%4px;"
                    "background:qradialgradient(spread:pad,cx:0.5,cy:0.5,radius:0.5,fx:0.5,fy:0.5,stop:0.6 #FFFFFF,stop:0.8 %1);}")
         .arg(handleColor).arg(handleWidth).arg(handleOffset).arg(handleRadius).arg(borderColor);

    // ============================================================================
    // 垂直滑块样式
    // ::vertical 选择器仅应用于垂直方向滑块
    // ============================================================================
    list << QString("QSlider::vertical{min-width:%1px;}").arg(sliderHeight * 2);
    list << QString("QSlider::groove:vertical{background:%1;width:%2px;border-radius:%3px;}")
         .arg(normalColor).arg(sliderHeight).arg(sliderRadius);
    list << QString("QSlider::add-page:vertical{background:%1;width:%2px;border-radius:%3px;}")
         .arg(grooveColor).arg(sliderHeight).arg(sliderRadius);
    list << QString("QSlider::sub-page:vertical{background:%1;width:%2px;border-radius:%3px;}")
         .arg(normalColor).arg(sliderHeight).arg(sliderRadius);
    list << QString("QSlider::handle:vertical{border:1px solid %5;height:%2px;margin-left:-%3px;margin-right:-%3px;border-radius:%4px;"
                    "background:qradialgradient(spread:pad,cx:0.5,cy:0.5,radius:0.5,fx:0.5,fy:0.5,stop:0.6 #FFFFFF,stop:0.8 %1);}")
         .arg(handleColor).arg(handleWidth).arg(handleOffset).arg(handleRadius).arg(borderColor);

    // 应用全局样式表
    qApp->setStyleSheet(list.join(""));
}
