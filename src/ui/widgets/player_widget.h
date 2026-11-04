#ifndef PLAYER_WIDGET_H
#define PLAYER_WIDGET_H

// ============================================================================
// player_widget.h - PlayerWidget：一路通道的外壳（GL 画面 + OSD + 围栏 + ⊕）
// ============================================================================
//
// 【所属模块】src/ui/widgets。它是"一路视频"的全部界面装配：把 GLVideoWidget
//   （零拷贝渲染）、FenceOverlay（围栏线框）、占位提示与放大钮叠在同一个
//   QGridLayout 单元 (0,0) 上，叠放次序与鼠标穿透的取舍见 .cpp 文件头。
// 【所有权】本窗体 new 并 **delete** 以下全部成员：decoder_（非 Qt 父子关系，
//   构造时 new、析构里显式 delete）、video_widget_/overlayLabel/placeholder_/
//   fenceOverlay_/expandBtn_/pollTimer_（都挂 this 为 parent，随 QObject 树析构）。
//   ⇒ 外部拿 decoder()/fenceOverlay() 只能"用"，绝不能 delete，也别缓存裸指针
//     跨过本窗体的生命周期（切通道/换流时本窗体可能被销毁）。
// 【线程】本类全部方法只可在 GUI 线程调用。跨线程的唯一交点是构造里那条
//   QueuedConnection：decoder_ 的解码线程 emit frameReady(RenderFrame) →
//   GLVideoWidget::onFrameReady 在 GUI 线程的事件循环里执行，
//   所以 EGL/GL 调用永远不落到解码线程（详见 gl_video_widget.cpp 文件头）。
// 【停机顺序契约】析构函数会先 decoder_->stop()（同步 join 解码线程）再删对象，
//   外部提前停流请用 signalStop()/waitForStop() 这一对（原因见下面注释）。
// ============================================================================

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
    作用：按"一条流"打开视频源（正式路径，参数来自 video.sources[]）
    说明：与上面那个重载的唯一区别是**带着打开参数**（type/尺寸/帧率/输入格式/
          是否硬解/loop/超时/重连）。地址在 source.url 里，空就什么都不做。
    ====================================================
    */
    void open(const VideoSourceCfg &source, int channel = 0);

    /*
    ====================================================
    作用：停止解码器
    说明：安全停止视频解码，释放相关资源
    ====================================================
    */
    void stopDecoder();
    // 两阶段停止：先 signalStop 通知退出（不阻塞），再 waitForStop 等待
    // 拆成两步是为了**四路并行退出**：先对每一路各 signalStop()，再逐个
    // waitForStop()，总耗时≈最慢那一路，而不是四路串相加（调用点 frmMain::closeEvent）。
    // ⚠ waitForStop 会阻塞调用线程（有界等待，见 ffmpeg_video_decoder.h 的 isLoopAlive：
    //   超时后会遗弃解码线程），所以只允许在关闭/热更新这类"本来就要停画面"的流程里调，
    //   普通槽函数里调它等于把 GUI 线程挂住、四路 paintGL 全停。
    // stopDecoder() 是旧的"一步到位"版本（内部 signal+wait），新代码优先用上面这一对。
    void signalStop() { if (decoder_) decoder_->signalStop(); }
    void waitForStop() { if (decoder_) decoder_->waitForStop(); }

    /* 
    ====================================================
    作用：获取解码器指针
    说明：提供对内部解码器的访问，用于外部控制
    返回值：FFmpegVideoDecoder指针
    【约束】所有权仍在 PlayerWidget：调用方不得 delete、不得跨本窗体生命周期缓存；
    构造后即可用（decoder_ 在构造里就 new 好，不是懒的），返回恒非空。
    返回它是因为外部要连它的信号（frameReady/statusChanged 都已经在内部连过了，
    外部通常只还需要 error 与推理相关的信号）。
    ====================================================
    */
    FFmpegVideoDecoder* decoder() const { return decoder_; }
    // 围栏叠加层：同上，所有权在本窗体，外部只读/只连信号。
    geofence::FenceOverlay* fenceOverlay() const { return fenceOverlay_; }
    // 本窗体负责的通道号（0 起）。必须在 open() **之前**调：
    // "未启用/没地址"两种占位文案是在这里就地查配置的，晚调就等于让占位先闪一帧错号。
    // 同时把通道号转给围栏层——它按通道取自己那份围栏数据，两者不同步会出现
    // "画面是通道 2、围栏画的是通道 1"这种最难查的错。
    // 参数范围：0..通道总数-1；-1 是构造后的初值，表示"还没分配"，此时不显示任何文案。
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
    // 设置放大状态：只改 ⊕/⊖ 文案，**不**触发任何布局动作。
    // 真正把这路摆到整屏的是调用方（frmMain 的视图切换），它负责成对调用：
    // 谁被放大、谁被收回，本类不知道其他三路的存在。
    void setExpanded(bool expanded);
    bool isExpanded() const { return expanded_; }

protected:
    // ⊕ 是靠手动 move() 定位的悬浮件（不在布局里，见 setupExpandButton），
    // 所以每次尺寸变化都要重算右上角位置；10px 是留给指肚的边距，别缩到 0。
    void resizeEvent(QResizeEvent *event) override;

private:
    // 创建右上角那颗 48x48 的 ⊕（T4 档）并 raise()。
    // 只在构造里调一次：它 setFixedSize + 内联 QSS（含必须显式的 padding:0px），
    // 重复调会再造一颗悬浮钮压在旧的那颗上。
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
    // ⊕ 的点击：只发 btnClicked("expand") 这一个字符串，不在这里改任何布局。
    // 用固定 objectName 而不是 sender()->objectName()，是因为外部按名字分派，
    // 本窗体只有这一颗钮会走到这里。
    void onExpandClicked();
    /* 
    ====================================================
    作用：处理按钮点击槽函数
    说明：响应用户界面按钮点击事件
    NOTE: 全仓没有任何 connect 指向这颗**无参**槽（⊕ 走的是 onExpandClicked），
    它是 blacksoft"一堆按钮共用一个槽"写法带过来的。真要复用得先注意两点：
    它用 sender() 取按钮且不判空（直连非按钮控件即段错误）；发出去的 objectName
    取决于调用方给按钮起的名字，而本窗体只给 expandBtn_ 起了名。
    ====================================================
    */
    void btnClicked();

Q_SIGNALS:    
    /* 
    ====================================================
    作用：按钮点击信号
    说明：发送按钮点击事件，通知其他组件
    参数：objName - 被点击按钮的对象名称
    【消费者】目前只有 frmVideoWindow（四路各转一次到它自己的 btnClicked(QString) 槽）。
    本窗体唯一会发出去的值是 "expand"（由 onExpandClicked 发），所以外部 switch
    这个名字就够了；再加悬浮钮时要记得外部也得跟着认新名字。
    ====================================================
    */
    void btnClicked(const QString &objName);

};

#endif