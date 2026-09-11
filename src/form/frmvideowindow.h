#ifndef FRMVIDEOWINDOW_H
#define FRMVIDEOWINDOW_H

/**
 * @file frmvideowindow.h
 * @brief 视频监控窗口 - 4路视频2x2网格显示
 *
 * 作用：定义视频监控窗口类，提供4路视频通道的管理和显示功能。
 *       每个通道对应一个 PlayerWidget，支持独立播放和控制。
 */

#include <QWidget>

// 前向声明播放器控件类，避免不必要的头文件包含
class PlayerWidget;

// Qt UI 命名空间，存放由 Qt Designer 生成的界面类
namespace Ui {
class frmVideoWindow;
}

// ============================================================================
// frmVideoWindow 类 —— 视频监控窗口
// ============================================================================
// 作用：管理4个 PlayerWidget 组成的 2x2 网格，提供通道选择和视频打开接口。
//       该类继承自 QWidget，是一个 Qt 控件。
// ============================================================================
class frmVideoWindow : public QWidget
{
    Q_OBJECT  // 启用 Qt 信号与槽机制

public:
    /**
     * @brief 构造函数
     * @param parent 父控件指针，默认为 nullptr
     * 作用：初始化视频窗口，创建 UI 并调用 initForm 完成信号连接
     */
    explicit frmVideoWindow(QWidget *parent = 0);

    /**
     * @brief 析构函数
     * 作用：释放4个播放器控件和 UI 对象的内存
     */
    ~frmVideoWindow();

    /**
     * @brief 获取指定通道的播放器控件
     * @param ch 通道号（0-3），对应 videoWindow1~4
     * @return PlayerWidget 指针，通道号无效时返回 nullptr
     * 作用：根据通道号返回对应的播放器控件，方便外部操作指定通道
     */
    PlayerWidget *playerWidget(int ch);

    /**
     * @brief 打开视频到指定通道
     * @param ch   通道号（0-3）
     * @param path 视频文件路径
     * 作用：将指定路径的视频文件加载到对应通道的播放器中进行解码显示
     */
    void openVideo(int ch, const QString &path);

private:
    void setExpandedMode(int channel, bool expanded);

    Ui::frmVideoWindow *ui;  // Qt Designer 生成的界面对象指针
    int expandedChannel_ = -1;  // 当前放大的通道，-1表示无放大

private slots:
    /** @brief 初始化表单：连接信号与槽 */
    void initForm();
    /** @brief 处理播放器按钮点击事件 */
    void btnClicked(const QString &objName);
};

#endif // FRMVIDEOWINDOW_H
