#ifndef CUSTOMSTYLE_H
#define CUSTOMSTYLE_H

// ============================================================================
// 自定义样式类头文件
// 提供Qt应用程序全局样式配置功能
// 支持自定义字体大小、控件尺寸、滑块样式等
// ============================================================================

#include <QObject>

// ============================================================================
// CustomStyle - 自定义样式类
// 使用Qt样式表(QSS)机制实现全局UI样式配置
// ============================================================================
class CustomStyle
{
public:
    /**
     * @brief 初始化全局样式
     * 设置应用程序的字体大小、单选框/复选框尺寸、滑块高度等
     * 
     * @param fontSize [in] 全局字体大小（像素）
     * @param radioButtonSize [in] 单选框指示器尺寸
     * @param checkBoxSize [in] 复选框指示器尺寸
     * @param sliderHeight [in] 滑块高度
     */
    static void initStyle(int fontSize = 15, int radioButtonSize = 18, int checkBoxSize = 16, int sliderHeight = 13);
};

#endif // CUSTOMSTYLE_H
