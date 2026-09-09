#include "player_widget.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QWidget>
#include <QString>
#include <QList>
#include <QGridLayout>
#include <QIcon>
#include <QSize>
#include <QPainter>
#include <QPen>
#include <QRect>
#include <QPoint>
#include <QDebug>
#include <QToolTip>
#include <QTextBrowser>
#include <QDateTime>
#include <QScrollBar>

/* 
====================================================
作用：播放器界面构造函数
说明：初始化所有UI组件，建立信号槽连接
====================================================
*/
PlayerWidget::PlayerWidget(QWidget* parent)
    : QWidget(parent)
{  
    // 创建主水平布局
    QGridLayout* mainLayout = new QGridLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);  // 减少主布局边距适配嵌入式屏幕
    mainLayout->setSpacing(0);  // 减少主布局间距适配嵌入式屏幕

     // 创建视频解码器实例
    decoder_ = new FFmpegVideoDecoder();

    // 创建OpenGL视频显示组件
    video_widget_ = new GLVideoWidget(this);
    video_widget_->setContentsMargins(0,0,0,0);

    // 创建覆盖层标签（用于显示OSD信息）
    overlayLabel = new QLabel(this);
    overlayLabel->setText("OSD 叠加");
    overlayLabel->setAlignment(Qt::AlignCenter);
    overlayLabel->setStyleSheet(
        "QLabel {"
        "   background: transparent;"  // 透明背景
        "   color: white;"  // 白色文字
        "   font-size: 24px;"  // 字体大小
        "   font-weight: bold;"  // 粗体
        "}"
    );
    // 设置鼠标事件穿透，使点击能传递到底层组件
    overlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlayLabel->setVisible(false);  // 初始隐藏

    // 将组件添加到布局
    mainLayout->addWidget(video_widget_, 0, 0);  // 视频显示组件
    mainLayout->addWidget(overlayLabel, 0, 0);   // 覆盖层


    /* 
    ====================================================
    信号槽连接（跨线程通信）
    说明：使用Qt::QueuedConnection实现线程间安全通信
    ====================================================
    */
    // 解码器帧就绪信号 -> 视频显示组件槽函数
    connect(decoder_, &FFmpegVideoDecoder::frameReady,
            video_widget_, &GLVideoWidget::onFrameReady,
            Qt::QueuedConnection);  // 使用队列连接，确保线程安全


    // 解码器错误信号 -> 错误处理槽函数
    connect(decoder_, &FFmpegVideoDecoder::error, this, [](QString msg) {
        qWarning() << "Decoder error:" << msg;
    });

    
}

/* 
====================================================
作用：打开视频源
说明：设置解码器频道并启动解码
参数：url - 视频源地址，channel - 频道号
====================================================
*/
void PlayerWidget::open(std::string url, int channel)
{
    decoder_->setChannel(channel);  // 设置解码器频道
    decoder_->start(QString::fromStdString(url));  // 启动解码
}

/* 
====================================================
作用：停止解码器
说明：安全停止视频解码过程
====================================================
*/
void PlayerWidget::stopDecoder()
{
    decoder_->stop();
}

/* 
====================================================
作用：处理按钮点击事件
说明：发送按钮点击信号，通知其他组件
====================================================
*/
void PlayerWidget::btnClicked()
{
    QPushButton *btn = (QPushButton *)sender();  // 获取发送信号的按钮
    Q_EMIT btnClicked(btn->objectName());  // 发送按钮名称信号
}

/* 
====================================================
作用：析构函数
说明：释放所有资源，停止解码器
====================================================
*/
PlayerWidget::~PlayerWidget() {
     decoder_->stop();  // 停止解码器
}