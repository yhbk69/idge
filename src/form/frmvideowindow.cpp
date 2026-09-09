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

// ============================================================================
// 构造函数
// ============================================================================
// 作用：初始化视频监控窗口，创建 UI 布局并调用 initForm 完成信号连接
// ============================================================================
frmVideoWindow::frmVideoWindow(QWidget *parent) : QWidget(parent), ui(new Ui::frmVideoWindow)
{
    ui->setupUi(this);       // 加载 Qt Designer 设计的界面布局
    this->initForm();        // 初始化信号连接
}

// ============================================================================
// 析构函数
// ============================================================================
// 作用：手动释放4个视频播放器控件和 UI 对象，防止内存泄漏
//       注意：由于播放器控件是 UI 管理的子对象，需逐个 delete
// ============================================================================
frmVideoWindow::~frmVideoWindow()
{
    delete ui->videoWindow1;  // 释放通道1的播放器
    delete ui->videoWindow2;  // 释放通道2的播放器
    delete ui->videoWindow3;  // 释放通道3的播放器
    delete ui->videoWindow4;  // 释放通道4的播放器
    delete ui;                // 释放 UI 对象
}

// ============================================================================
// initForm —— 初始化视频窗口
// ============================================================================
// 作用：只做信号连接，不在这里打开视频。
//       视频由 frmMain 在 ConfigManager 加载完成后调用 openVideo() 打开。
// ============================================================================
void frmVideoWindow::initForm()
{
    // 连接4个播放器的按钮点击信号到本窗口的 btnClicked 槽函数
    // 当播放器内部按钮被点击时，会触发此信号链
    connect(ui->videoWindow1, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow2, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow3, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow4, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
}

// ============================================================================
// openVideo —— 打开视频到指定通道
// ============================================================================
// 作用：根据通道号找到对应的播放器控件，并将视频文件路径传递给它进行解码播放
// @param ch   通道号（0-3）
// @param path 视频文件路径
// ============================================================================
void frmVideoWindow::openVideo(int ch, const QString &path)
{
    PlayerWidget *pw = playerWidget(ch);   // 根据通道号获取播放器指针
    if (pw && !path.isEmpty()) {           // 确保播放器有效且路径非空
        pw->open(path.toStdString(), ch);  // 将 QString 转为 std::string 后打开视频
    }
}

// ============================================================================
// btnClicked —— 播放器按钮点击事件处理
// ============================================================================
// 作用：当播放器内部按钮被点击时，在界面 label 上显示当前点击的控件和按钮信息
//       通过 sender() 获取触发信号的对象，从而确定是哪个通道的播放器
// ============================================================================
void frmVideoWindow::btnClicked(const QString &objName)
{
    // sender() 返回发送信号的对象，将其转换为 PlayerWidget 类型
    PlayerWidget *videoWindow = (PlayerWidget *)sender();
    // 格式化显示文本：包含控件名称和按钮名称
    QString str = QString("当前单击了控件 %1 的按钮 %2").arg(videoWindow->objectName()).arg(objName);
    ui->label->setText(str);  // 更新界面标签文本
}

// ============================================================================
// playerWidget —— 获取指定通道的播放器控件
// ============================================================================
// 作用：根据通道号（0-3）返回对应的 PlayerWidget 指针
//       通道号与 UI 控件的对应关系：0→videoWindow1, 1→videoWindow2, ...
// @param ch 通道号（0-3）
// @return 对应通道的 PlayerWidget 指针，无效通道返回 nullptr
// ============================================================================
PlayerWidget *frmVideoWindow::playerWidget(int ch)
{
    switch (ch) {
        case 0: return ui->videoWindow1;  // 通道0 → 第一个播放器
        case 1: return ui->videoWindow2;  // 通道1 → 第二个播放器
        case 2: return ui->videoWindow3;  // 通道2 → 第三个播放器
        case 3: return ui->videoWindow4;  // 通道3 → 第四个播放器
        default: return nullptr;          // 无效通道返回空指针
    }
}
