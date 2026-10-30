// player_widget.cpp —— 单通道播放器窗体：GL 视频 + OSD + 围栏叠加 + 放大按钮的组合
//
// 【组合关系】四层叠加于同一 QGridLayout 单元 (0,0)，加入顺序即叠放顺序：
//   GLVideoWidget（底层，零拷贝渲染）→ overlayLabel（OSD，鼠标穿透）
//   → placeholder_（"视频待接入"提示，鼠标穿透；刻意排在围栏层**之下**，
//     这样没接上视频时存量的围栏线框仍可看见、可删可改）
//   → FenceOverlay（围栏线框）→ expandBtn（悬浮按钮，raise 到最上层并手动定位右上角）。
//   解码线程产物经 QueuedConnection 信号投递到 GLVideoWidget 的槽，
//   保证 EGL/GL 调用始终发生在 GUI 线程（GL 上下文线程亲和）。
#include "player_widget.h"
#include "theme.h"
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
#include <QTimer>
#include "fence_overlay.h"
#include "ConfigManager.h"

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

     // 创建视频解码器实例（析构时 delete）
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

    // ------------------------------------------------------------------------
    // "视频待接入"占位（1b）
    // ------------------------------------------------------------------------
    // 同一个 (0,0) 单元里的第 3 层：压在视频上、但在围栏层**之前**加入布局，
    // 于是存量的围栏线框仍然看得见 —— 没接上视频时最想干的活恰恰是"照着画面
    // 之外的位置把围栏挪一挪/删掉重画"，把它盖严实就成了死区。
    // 鼠标穿透：⊕ 与围栏绘制都不该被一块提示挡住。
    placeholder_ = new QLabel(this);
    placeholder_->setAlignment(Qt::AlignCenter);
    placeholder_->setTextFormat(Qt::PlainText);   // 解码器报错串可能含尖括号，绝不能吃成富文本
    placeholder_->setWordWrap(true);
    placeholder_->setAttribute(Qt::WA_TransparentForMouseEvents);
    placeholder_->setStyleSheet(
        QString("QLabel { background: rgba(14, 14, 22, 190); color: %1;"
                "  font-size:%2px; padding: 12px; }")
            .arg(theme::TEXT_MUTED)
            .arg(theme::FS_CARD));
    mainLayout->addWidget(placeholder_, 0, 0);

    // 创建电子围栏覆盖层
    fenceOverlay_ = new geofence::FenceOverlay(this);
    mainLayout->addWidget(fenceOverlay_, 0, 0);  // 围栏覆盖层（最上层）

    // 初始化放大按钮（内部 raise()，天然在占位与围栏之上）
    setupExpandButton();

    // 等首帧的轮询：只在占位可见且还没出过帧时跑，见 player_widget.h 的说明
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(500);
    connect(pollTimer_, &QTimer::timeout, this, [this]() {
        if (video_widget_->hasFrame()) {
            hasFrame_ = true;
            updatePlaceholder();
        }
    });


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

    // 视频内容矩形（扣掉黑边的那块画面）变化 -> 围栏叠加层的换算基准。
    // 视图切换、全屏、窗口缩放、首帧到位都会走到；围栏存的是归一化坐标，
    // 这里变的只是"画在哪几个像素"，不是围栏本身，所以不需要碰围栏数据。
    connect(video_widget_, &GLVideoWidget::contentRectChanged,
            fenceOverlay_, &geofence::FenceOverlay::setContentRect);


    // 解码器错误信号 -> 占位文案
    // 这里刻意"既 warning 又刷占位"：原来只有一个只 qWarning 的 lambda，
    // 现场表现为"格子黑着、什么也没说"，只有 SSH 上去看日志才知道流没连上。
    // msg 只取首行并截断：ffmpeg 的错误串动辄几百字节，一行提示区放不下也不会更好读。
    connect(decoder_, &FFmpegVideoDecoder::error, this, [this](const QString &msg) {
        qWarning() << "Decoder error:" << msg;
        errorText_ = msg.section('\n', 0, 0).left(80);
        signalLost_ = false;
        updatePlaceholder();
    });

    // 在线状态：连上就清掉上一次的失败/中断，断了才提示"信号中断"
    // 只认本通道的广播（解码器把 statusChanged 发给了 frmMain 的 AlarmManager，
    // 这里是第二个消费者，不判 ch 会把四路的态搅在一起）。
    connect(decoder_, &FFmpegVideoDecoder::statusChanged, this,
            [this](int ch, int online) {
                if (ch != channel_) return;
                if (online != 0) {
                    errorText_.clear();
                    signalLost_ = false;
                } else {
                    signalLost_ = true;
                }
                updatePlaceholder();
            });

    updatePlaceholder();   // 首帧前的初值：未启用 / 未配置地址 当场就能判出来


    
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
    // 触屏档 T4：48x48 ≈ 5.4mm（原 32x32 只有 3.6mm，四路画面里最容易误触的钮）
    expandBtn_->setFixedSize(theme::TOUCH_ICON_BTN, theme::TOUCH_ICON_BTN);
    expandBtn_->setCursor(Qt::PointingHandCursor);
    
    // 初始状态：放大图标
    expandBtn_->setText("⊕");
    
    // 半透明悬浮样式
    // padding 必须显式清 0：全局 .QPushButton 现在是触屏档 padding-left/right:14px，
    // 48x48 的固定钮被它吃掉后 ⊕ 只剩 46px 可用却需要 50px（探针实测会截字）。
    // 命中区已经是 48x48 ≈ 5.4mm，不再需要 padding 补宽。
    expandBtn_->setStyleSheet(
        "QPushButton {"
        "   background-color: rgba(0, 0, 0, 120);"
        "   border: none;"
        "   border-radius: 4px;"
        "   padding: 0px;"
        "   color: white;"
        "   font-size:22px;"
        "}"
        "QPushButton:hover {"
        "   background-color: rgba(0, 0, 0, 180);"
        "}"
    );
    
    // 按钮位置：右上角，留10px边距（尺寸由 resizeEvent 按 width() 跟随）
    expandBtn_->setGeometry(0, 0, theme::TOUCH_ICON_BTN, theme::TOUCH_ICON_BTN);
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
    // 报警视频录制参数一次性注入：编码器在 decodeLoop 打开流后才建，
    // 运行期改配置不会重建编码器，所以这里不做热更新。
    ConfigManager &cfg = ConfigManager::instance();
    RecorderConfig recording;
    recording.enabled = cfg.recordingEnabled();
    recording.codec = cfg.recordingCodec();
    recording.bufferSeconds = cfg.recordingBufferSeconds();
    recording.bitRate = cfg.recordingBitRate();
    recording.gopSeconds = cfg.recordingGopSeconds();
    decoder_->setRecordingConfig(recording);

    // 换流即重新走一遍"正在接入… → 出帧"的态：上一路的失败残留（errorText_/
    // signalLost_）不清掉的话，新流明明连上了占位还挂着"接入失败"。
    channel_ = channel;
    hasFrame_ = false;
    errorText_.clear();
    signalLost_ = false;
    updatePlaceholder();

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
析构顺序契约：先 stop() 停解码线程，再 delete 解码器对象，
最后让子 widget 析构。防止子 widget 析构后仍有 frameReady
队列信号在途——那会让待处理的 RenderFrame 携带无人签收的
dup fd（泄漏），甚至投递到已析构对象。
====================================================
*/
PlayerWidget::~PlayerWidget() {
    if (decoder_) {
        decoder_->stop();   // 停止解码器（同步 join）
        delete decoder_;    // 释放解码器对象
        decoder_ = nullptr;
    }
}

void PlayerWidget::setChannel(int channel)
{
    channel_ = channel;
    fenceOverlay_->setChannel(channel);
    // frmMain 在 ConfigManager::load() 之后、开流之前就把通道号配好了，
    // 所以"未启用/没地址"这两种态在这里就能判出来，不用等任何信号。
    updatePlaceholder();
}

// ============================================================================
// updatePlaceholder —— 决定这一格该显示什么提示（不显示=画面正常）
// ============================================================================
// 判序是有意的，从"配置层面根本不会有流"往"流本身出了问题"排：
//   1) 未启用          —— 配置说这路不开，任何运行期状态都不该盖过它
//   2) 视频待接入       —— 启用了但没地址，同上，且这条正是需求点名的文案
//   3) 接入失败：msg    —— 开过流、报过错（比"信号中断"具体，优先）
//   4) 信号中断         —— 曾经在线/开过流，解码循环保了不在线
//   5) 正在接入…       —— 已 open()，还没等到第一帧
//   6) 空串            —— 出过帧且没报错，隐藏占位
// channel_ 还是 -1（构造与 setChannel 之间的极短窗口）时不显示任何文案，
// 免得四路在启动瞬间统一闪一遍"通道0 未启用"之类的错号。
// ============================================================================
void PlayerWidget::updatePlaceholder()
{
    if (!placeholder_) return;

    QString text;
    if (channel_ >= 0) {
        ConfigManager &cfg = ConfigManager::instance();
        if (!cfg.channelEnabled(channel_))
            text = QString("通道%1 未启用").arg(channel_ + 1);
        else if (cfg.channelSource(channel_).isEmpty())
            text = QString("通道%1 视频待接入（未配置地址）").arg(channel_ + 1);
        else if (!errorText_.isEmpty())
            text = QString("通道%1 接入失败：%2").arg(channel_ + 1).arg(errorText_);
        else if (signalLost_)
            text = QString("通道%1 信号中断").arg(channel_ + 1);
        else if (!hasFrame_)
            text = QString("通道%1 正在接入…").arg(channel_ + 1);
    }

    if (text.isEmpty()) {
        placeholder_->hide();
        pollTimer_->stop();          // 画面正常，等首帧的轮询就此停掉
    } else {
        placeholder_->setText(text);
        placeholder_->show();
        // 只在"还没出过帧"时才需要轮询；已经出过帧的态（信号中断）是等事件，
        // 轮询 hasFrame() 永远是真，白跑。
        if (!hasFrame_) {
            if (!pollTimer_->isActive()) pollTimer_->start();
        } else {
            pollTimer_->stop();
        }
    }
    // ⊕ 必须一直在最上面：占位块是整格的，鼠标虽然穿透，但视觉上被压住
    // 就等于这一路没法放大（触屏上尤其像"按钮没了"）。
    if (expandBtn_) expandBtn_->raise();
}