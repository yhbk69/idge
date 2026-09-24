#include "appdata.h"
#include "qthelper.h"

// ============================================================================
// 静态成员初始化
// 定义应用程序的默认界面参数
// ============================================================================
QString AppData::TitleFlag = "";           // 默认标题为空
int AppData::RowHeight = 25;              // 默认行高25像素
int AppData::RightWidth = 250;            // 默认右侧宽度250像素
int AppData::FormWidth = 1200;            // 默认窗体宽度1200像素
int AppData::FormHeight = 750;            // 默认窗体高度750像素

// ============================================================================
// checkRatio - 根据屏幕分辨率调整界面参数
// 当屏幕宽度>=1440像素时，确保界面参数不小于推荐值
// 避免在高分辨率屏幕上界面元素过小
// ============================================================================
void AppData::checkRatio()
{
    // 获取屏幕宽度
    int width = QtHelper::deskWidth();

    if (width >= 1440) {
        // 高分辨率屏幕：确保各项参数不低于最小值
        RowHeight = RowHeight < 25 ? 25 : RowHeight;
        RightWidth = RightWidth < 220 ? 220 : RightWidth;
        FormWidth = FormWidth < 1200 ? 1200 : FormWidth;
        FormHeight = FormHeight < 800 ? 800 : FormHeight;
    }
}
