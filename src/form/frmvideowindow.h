#ifndef FRMVIDEOWINDOW_H
#define FRMVIDEOWINDOW_H

/**
 * @file frmvideowindow.h
 * @brief 视频监控窗口 - 4路视频2x2网格显示
 */

#include <QWidget>

class PlayerWidget;

namespace Ui {
class frmVideoWindow;
}

class frmVideoWindow : public QWidget
{
    Q_OBJECT

public:
    explicit frmVideoWindow(QWidget *parent = 0);
    ~frmVideoWindow();

    /**
     * @brief 获取指定通道的播放器控件
     * @param ch 通道号（0-3），对应 videoWindow1~4
     * @return PlayerWidget 指针，通道号无效时返回 nullptr
     */
    PlayerWidget *playerWidget(int ch);

    /**
     * @brief 打开视频到指定通道
     * @param ch   通道号（0-3）
     * @param path 视频文件路径
     */
    void openVideo(int ch, const QString &path);

private:
    Ui::frmVideoWindow *ui;

private slots:
    void initForm();
    void btnClicked(const QString &objName);
};

#endif // FRMVIDEOWINDOW_H
