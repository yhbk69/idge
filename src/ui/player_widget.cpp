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

PlayerWidget::PlayerWidget(QWidget* parent)
    : QWidget(parent)
{  
    // 创建主水平布局
    QGridLayout* mainLayout = new QGridLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);  // 减少主布局边距适配嵌入式屏幕
    mainLayout->setSpacing(0);  // 减少主布局间距适配嵌入式屏幕

     // 解码器
    decoder_ = new FFmpegVideoDecoder();

    video_widget_ = new GLVideoWidget(this);
    video_widget_->setContentsMargins(0,0,0,0);

    overlayLabel = new QLabel(this);
    overlayLabel->setText("OSD 叠加");
    overlayLabel->setAlignment(Qt::AlignCenter);
    overlayLabel->setStyleSheet(
        "QLabel {"
        "   background: transparent;"
        "   color: white;"
        "   font-size: 24px;"
        "   font-weight: bold;"
        "}"
    );
    overlayLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    overlayLabel->setVisible(false);

    mainLayout->addWidget(video_widget_, 0, 0);
    mainLayout->addWidget(overlayLabel, 0, 0);


    // 信号槽连接（跨线程）
    connect(decoder_, &FFmpegVideoDecoder::frameReady,
            video_widget_, &GLVideoWidget::onFrameReady,
            Qt::QueuedConnection);


    connect(decoder_, &FFmpegVideoDecoder::error, this, [](QString msg) {
        qWarning() << "Decoder error:" << msg;
    });

    
}

void PlayerWidget::open(std::string url)
{
    decoder_->start(QString::fromStdString(url));

}

void PlayerWidget::btnClicked()
{
    QPushButton *btn = (QPushButton *)sender();
    Q_EMIT btnClicked(btn->objectName());
}

PlayerWidget::~PlayerWidget() {
     decoder_->stop(); 
}