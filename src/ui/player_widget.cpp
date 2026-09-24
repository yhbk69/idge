// player_widget.cpp —— 单通道播放器窗体：GL 视频 + OSD + 围栏叠加 + 放大按钮的组合
//
// 【组合关系】三层叠加于同一 QGridLayout 单元 (0,0)：
//   GLVideoWidget（底层，零拷贝渲染）→ overlayLabel（OSD，鼠标穿透）
//   → FenceOverlay（围栏线框）→ expandBtn（悬浮按钮，手动定位右上角）。
//   解码线程产物经 QueuedConnection 信号投递到 GLVideoWidget 的槽，
//   保证 EGL/GL 调用始终发生在 GUI 线程（GL 上下文线程亲和）。
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
#include "fence_overlay.h"

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
    // ⚠ 所有权：decoder_ 以 new 创建且未传 QObject parent——它不在本
    //   widget 的子对象树上，~PlayerWidget 只 stop() 不 delete()。
    //   每个 PlayerWidget 会泄漏一个解码器对象（含 FFmpeg 上下文），
    //   通道反复重建时累积明显；依赖其内部自行释放时需逐一核实。
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
        "   font-size:22px;"  // 字体大小
        "   font-weight: bold;"  // 粗体
        "}"
    );
    // 设置鼠标事件穿透，使点击能传递到底层组件
    overlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlayLabel->setVisible(false);  // 初始隐藏

    // 将组件添加到布局
    mainLayout->addWidget(video_widget_, 0, 0);  // 视频显示组件
    mainLayout->addWidget(overlayLabel, 0, 0);   // 覆盖层

    // 创建电子围栏覆盖层
    fenceOverlay_ = new geofence::FenceOverlay(this);
    mainLayout->addWidget(fenceOverlay_, 0, 0);  // 围栏覆盖层（最上层）

    // 初始化放大按钮
    setupExpandButton();


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
作用：窗口大小改变事件
说明：保持放大按钮在右上角位置
====================================================
*/
void PlayerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (expandBtn_) {
        int x = width() - expandBtn_->width() - 10;
        int y = 10;
        expandBtn_->move(x, y);
    }
}

/* 
====================================================
作用：设置放大按钮
说明：创建右上角悬浮的放大/缩小按钮
====================================================
*/
void PlayerWidget::setupExpandButton()
{
    expandBtn_ = new QPushButton(this);
    expandBtn_->setObjectName("expandBtn");
    expandBtn_->setFixedSize(32, 32);
    expandBtn_->setCursor(Qt::PointingHandCursor);
    
    // 初始状态：放大图标
    expandBtn_->setText("⊕");
    
    // 半透明悬浮样式
    expandBtn_->setStyleSheet(
        "QPushButton {"
        "   background-color: rgba(0, 0, 0, 120);"
        "   border: none;"
        "   border-radius: 4px;"
        "   color: white;"
        "   font-size:18px;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(0, 0, 0, 180);"
        "}"
    );
    
    // 按钮位置：右上角，留10px边距
    expandBtn_->setGeometry(0, 0, 32, 32);
    expandBtn_->raise();  // 显示在最上层
    
    connect(expandBtn_, &QPushButton::clicked, this, &PlayerWidget::onExpandClicked);
}

/* 
====================================================
作用：放大按钮点击处理
说明：发送放大请求信号给父窗口
====================================================
*/
void PlayerWidget::onExpandClicked()
{
    Q_EMIT btnClicked("expand");
}

/* 
====================================================
作用：设置放大状态
说明：更新按钮文本和状态
参数：expanded - true为放大，false为缩小
====================================================
*/
void PlayerWidget::setExpanded(bool expanded)
{
    expanded_ = expanded;
    if (expanded) {
        expandBtn_->setText("⊖");
    } else {
        expandBtn_->setText("⊕");
    }
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
⚠ 析构顺序契约：先 stop() 停解码线程，防止子 widget（video_widget_）
  析构后仍有 frameReady 队列信号在途——那会让待处理的 RenderFrame
  携带无人签收的 dup fd（泄漏），甚至投递到已析构对象。
  stop() 必须是同步 join 型接口才真正闭环。
====================================================
*/
PlayerWidget::~PlayerWidget() {
     decoder_->stop();  // 停止解码器
}

void PlayerWidget::setChannel(int channel)
{
    fenceOverlay_->setChannel(channel);
}