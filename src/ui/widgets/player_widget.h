#ifndef PLAYER_WIDGET_H
#define PLAYER_WIDGET_H


#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include "gl_video_widget.h"
#include "ffmpeg_video_decoder.h"
#include "fence_overlay.h"

/* 
====================================================
作用：播放器界面组件 - 提供视频播放的用户界面
说明：整合了视频显示、解码器和控制按钮
====================================================
*/
class PlayerWidget : public QWidget {
    Q_OBJECT

public:
    /* 
    ====================================================
    作用：构造函数
    说明：初始化播放器界面，创建必要的UI组件
    参数：parent - 父窗口指针
    ====================================================
    */
    explicit PlayerWidget(QWidget* parent = nullptr);

    /* 
    ====================================================
    作用：析构函数
    说明：释放播放器资源
    ====================================================
    */
    ~PlayerWidget();

    /* 
    ====================================================
    作用：打开视频源
    说明：开始解码并显示指定的视频流
    参数：url - 视频源地址（文件路径或网络流）
          channel - 频道号（用于多画面显示）
    ====================================================
    */
    void open(std::string url, int channel = 0);

    /*
    ====================================================
    作用：停止解码器
    说明：安全停止视频解码，释放相关资源
    ====================================================
    */
    void stopDecoder();
    // 两阶段停止：先 signalStop 通知退出（不阻塞），再 waitForStop 等待
    void signalStop() { if (decoder_) decoder_->signalStop(); }
    void waitForStop() { if (decoder_) decoder_->waitForStop(); }

    /* 
    ====================================================
    作用：获取解码器指针
    说明：提供对内部解码器的访问，用于外部控制
    返回值：FFmpegVideoDecoder指针
    ====================================================
    */
    FFmpegVideoDecoder* decoder() const { return decoder_; }
    geofence::FenceOverlay* fenceOverlay() const { return fenceOverlay_; }
    void setChannel(int channel);

    /*
    ====================================================
    作用：按当前配置与接入状态刷新"视频待接入"占位文案
    说明：公开是因为设置页热更新（frmMain::applyChannelConfigNow）改了本路
          enabled / 地址之后，本窗体收不到任何回调 —— 那两处正是占位文案
          唯一会读到新值的时机，不刷新的话界面上会一直挂着上一条状态。
    ====================================================
    */
    void updatePlaceholder();

public:
    // 设置放大状态
    void setExpanded(bool expanded);
    bool isExpanded() const { return expanded_; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupExpandButton();

    GLVideoWidget* video_widget_;  // OpenGL视频显示组件
    QLabel* overlayLabel;  // 覆盖层标签（用于显示叠加信息）
    geofence::FenceOverlay* fenceOverlay_;  // 电子围栏覆盖层
    FFmpegVideoDecoder* decoder_;  // FFmpeg视频解码器
    QPushButton* expandBtn_;  // 放大/缩小按钮
    bool expanded_ = false;  // 当前是否放大状态

    /*
    ====================================================
    "视频待接入"占位（1b）
    ----------------------------------------------------
    placeholder_  未接上视频时盖在画面上的整块提示
    channel_      本窗体负责的通道号（-1=还没分配，此时不显示任何文案）
    hasFrame_     本路是否已经出过帧（open() 会复位，换流后要重新等）
    errorText_    解码器最后一次报错的首行（空=没报过错）
    signalLost_   最后一次 statusChanged 是不在线
    pollTimer_    仅在"等第一帧"这个态里跑的轮询定时器
    说明：出帧没有走信号，是 500ms 轮询 video_widget_->hasFrame()。原因是
          RenderFrame 携带的是 dup 出来的 fd，所有权协议规定"每个副本恰好一次
          close"且唯一消费者是 GLVideoWidget::onFrameReady；给 frameReady 挂
          第二个槽会多投递一份副本、多出一个没人 close（或重复 close）的 fd。
          轮询只在占位可见时跑，画面正常后定时器是停的，不进每帧热路径。
    ====================================================
    */
    QLabel* placeholder_ = nullptr;
    int channel_ = -1;
    bool hasFrame_ = false;
    QString errorText_;
    bool signalLost_ = false;
    QTimer* pollTimer_ = nullptr;

private slots:
    void onExpandClicked();
    /* 
    ====================================================
    作用：处理按钮点击槽函数
    说明：响应用户界面按钮点击事件
    ====================================================
    */
    void btnClicked();

Q_SIGNALS:    
    /* 
    ====================================================
    作用：按钮点击信号
    说明：发送按钮点击事件，通知其他组件
    参数：objName - 被点击按钮的对象名称
    ====================================================
    */
    void btnClicked(const QString &objName);

};

#endif