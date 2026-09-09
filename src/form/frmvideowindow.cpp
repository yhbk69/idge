#pragma execution_character_set("utf-8")

/**
 * @file frmvideowindow.cpp
 * @brief 视频监控窗口 - 4路视频2x2网格显示
 *
 * 功能：
 *   - 4个 PlayerWidget 组成 2x2 网格
 *   - 每个通道独立解码和显示
 *   - 通过 openVideo(int ch, QString path) 从外部加载视频
 */

#include "frmvideowindow.h"
#include "ui_frmvideowindow.h"
#include <QTimer>
#include "player_widget.h"

frmVideoWindow::frmVideoWindow(QWidget *parent) : QWidget(parent), ui(new Ui::frmVideoWindow)
{
    ui->setupUi(this);
    this->initForm();
}

frmVideoWindow::~frmVideoWindow()
{
    delete ui->videoWindow1;
    delete ui->videoWindow2;
    delete ui->videoWindow3;
    delete ui->videoWindow4;
    delete ui;
}

/**
 * @brief 初始化视频窗口
 * 只做信号连接，不在这里打开视频。
 * 视频由 frmMain 在 ConfigManager 加载完成后调用 openVideo() 打开。
 */
void frmVideoWindow::initForm()
{
    // 连接4个播放器的按钮点击信号
    connect(ui->videoWindow1, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow2, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow3, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow4, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
}

/**
 * @brief 打开视频到指定通道
 * @param ch   通道号（0-3）
 * @param path 视频文件路径
 */
void frmVideoWindow::openVideo(int ch, const QString &path)
{
    PlayerWidget *pw = playerWidget(ch);
    if (pw && !path.isEmpty()) {
        pw->open(path.toStdString(), ch);
    }
}

void frmVideoWindow::btnClicked(const QString &objName)
{
    PlayerWidget *videoWindow = (PlayerWidget *)sender();
    QString str = QString("当前单击了控件 %1 的按钮 %2").arg(videoWindow->objectName()).arg(objName);
    ui->label->setText(str);
}

/**
 * @brief 获取指定通道的播放器控件
 * @param ch 通道号（0-3），对应 videoWindow1~4
 * @return PlayerWidget 指针，通道号无效时返回 nullptr
 */
PlayerWidget *frmVideoWindow::playerWidget(int ch)
{
    switch (ch) {
        case 0: return ui->videoWindow1;
        case 1: return ui->videoWindow2;
        case 2: return ui->videoWindow3;
        case 3: return ui->videoWindow4;
        default: return nullptr;
    }
}
