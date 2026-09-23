#pragma execution_character_set("utf-8")

/**
 * @file frmvideowindow.cpp
 * @brief 视频监控窗口 - 4路视频2x2网格显示
 *
 * 功能：
 *   - 4个 PlayerWidget 组成 2x2 网格
 *   - 每个通道独立解码和显示
 *   - 通过 openVideo(int ch, QString path) 从外部加载视频
 *   - 电子围栏工具栏：矩形/多边形绘制、删除、清空
 */

#include "frmvideowindow.h"
#include "ui_frmvideowindow.h"
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QButtonGroup>
#include <QFrame>
#include "player_widget.h"
#include "fence_overlay.h"
#include "fence_manager.h"

// ============================================================================
// 构造函数
// ============================================================================
// 作用：初始化视频监控窗口，创建 UI 布局并调用 initForm 完成信号连接
// ============================================================================
frmVideoWindow::frmVideoWindow(QWidget *parent) : QWidget(parent), ui(new Ui::frmVideoWindow)
{
    ui->setupUi(this);       // 加载 Qt Designer 设计的界面布局
    this->setupFenceToolbar();
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
// 作用：当播放器内部按钮被点击时，处理放大/缩小操作
//       通过 sender() 获取触发信号的对象，从而确定是哪个通道的播放器
// ============================================================================
void frmVideoWindow::btnClicked(const QString &objName)
{
    PlayerWidget *videoWindow = (PlayerWidget *)sender();
    
    if (objName == "expand") {
        // 判断当前是否已放大
        if (expandedChannel_ >= 0) {
            // 已放大，恢复四宫格
            setExpandedMode(expandedChannel_, false);
        } else {
            // 未放大，找到该通道号并放大
            int ch = -1;
            if (videoWindow == ui->videoWindow1) ch = 0;
            else if (videoWindow == ui->videoWindow2) ch = 1;
            else if (videoWindow == ui->videoWindow3) ch = 2;
            else if (videoWindow == ui->videoWindow4) ch = 3;
            
            if (ch >= 0) {
                setExpandedMode(ch, true);
            }
        }
    } else {
        // 其他按钮，保持原有逻辑
        QString str = QString("当前单击了控件 %1 的按钮 %2").arg(videoWindow->objectName()).arg(objName);
        ui->label->setText(str);
    }
}

// ============================================================================
// setExpandedMode —— 设置放大/缩小模式
// ============================================================================
// 作用：切换视频窗口的放大和缩小状态
//       放大时只显示选中的通道，缩小恢复2x2网格
// @param channel 要放大的通道号（0-3），-1表示恢复
// @param expanded true=放大，false=缩小
// ============================================================================
void frmVideoWindow::setExpandedMode(int channel, bool expanded)
{
    PlayerWidget* players[4] = {
        ui->videoWindow1, ui->videoWindow2,
        ui->videoWindow3, ui->videoWindow4
    };
    
    if (expanded) {
        // 放大模式：隐藏其他通道，只显示选中通道
        expandedChannel_ = channel;
        
        for (int i = 0; i < 4; i++) {
            if (i == channel) {
                players[i]->show();
                players[i]->setExpanded(true);
            } else {
                players[i]->hide();
            }
        }
        
        // 隐藏底部label
        if (ui->label) {
            ui->label->hide();
        }
    } else {
        // 恢复四宫格模式
        expandedChannel_ = -1;
        
        for (int i = 0; i < 4; i++) {
            players[i]->show();
            players[i]->setExpanded(false);
        }
        
        // 显示底部label
        if (ui->label) {
            ui->label->show();
        }
    }
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

// ============================================================================
// setupFenceToolbar —— 电子围栏工具栏（代码动态创建，不在 .ui 中）
// ============================================================================
// 作用：在 gridLayout 视频区下方插入一行工具栏：
//   通道切换按钮 ×4（28x24 可多选组，仅用于选中当前编辑通道，非互斥故
//   setExclusive(false)） | 矩形/多边形/删除（互斥绘制工具组
//   fenceDrawGroup_） | 清空（直接 FenceManager::clearChannel + 刷新叠加层）。
//   工具栏高 36px，按钮 12px 字体为侧边栏紧凑布局的经验尺寸。
// 布局手法：addWidget(toolbar, 2, 0, 1, 2) 占满两列；再把原第 2 行的
//   label（点击提示条）挪到第 3 行，避免改动 .ui 文件。
// ============================================================================
void frmVideoWindow::setupFenceToolbar()
{
    QFrame *toolbar = new QFrame(this);
    toolbar->setFrameStyle(QFrame::StyledPanel);
    toolbar->setFixedHeight(36);
    toolbar->setStyleSheet(
        "QFrame { background: #2a2a2a; border: 1px solid #444; }"
        "QPushButton { background: #3a3a3a; color: #ddd; border: 1px solid #555;"
        "  border-radius: 3px; padding: 2px 8px; font-size: 12px; }"
        "QPushButton:hover { background: #4a4a4a; }"
        "QPushButton:checked { background: #0078d4; color: white; }"
        "QLabel { color: #aaa; font-size: 12px; }"
    );

    QHBoxLayout *lay = new QHBoxLayout(toolbar);
    lay->setContentsMargins(6, 2, 6, 2);
    lay->setSpacing(4);

    lay->addWidget(new QLabel("通道:", toolbar));

    fenceToolGroup_ = new QButtonGroup(this);
    fenceToolGroup_->setExclusive(false);

    for (int i = 0; i < 4; ++i) {
        QPushButton *btn = new QPushButton(QString::number(i + 1), toolbar);
        btn->setCheckable(true);
        btn->setFixedSize(28, 24);
        if (i == 0) btn->setChecked(true);
        fenceChannelBtns_[i] = btn;
        fenceToolGroup_->addButton(btn, i);
        lay->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, i]() { onFenceChannelClicked(i); });
    }

    lay->addSpacing(10);
    lay->addWidget(new QLabel("|", toolbar));

    QPushButton *rectBtn = new QPushButton("矩形", toolbar);
    rectBtn->setObjectName("fenceRect");
    rectBtn->setCheckable(true);
    lay->addWidget(rectBtn);

    QPushButton *polyBtn = new QPushButton("多边形", toolbar);
    polyBtn->setObjectName("fencePoly");
    polyBtn->setCheckable(true);
    lay->addWidget(polyBtn);

    QPushButton *deleteBtn = new QPushButton("删除", toolbar);
    deleteBtn->setObjectName("fenceDelete");
    deleteBtn->setCheckable(true);
    lay->addWidget(deleteBtn);

    QPushButton *clearBtn = new QPushButton("清空", toolbar);
    clearBtn->setObjectName("fenceClear");
    lay->addWidget(clearBtn);

    lay->addStretch();

    fenceDrawGroup_ = new QButtonGroup(this);
    fenceDrawGroup_->setExclusive(true);
    fenceDrawGroup_->addButton(rectBtn, 0);
    fenceDrawGroup_->addButton(polyBtn, 1);
    fenceDrawGroup_->addButton(deleteBtn, 2);

    connect(rectBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(0); });
    connect(polyBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(1); });
    connect(deleteBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(2); });
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        geofence::FenceManager::instance().clearChannel(currentFenceChannel_);
        if (auto *pw = playerWidget(currentFenceChannel_))
            if (pw->fenceOverlay()) pw->fenceOverlay()->loadFences();
    });

    // 插入到 gridLayout 的第 2 行（视频下方，label 上方）
    ui->gridLayout->addWidget(toolbar, 2, 0, 1, 2);
    // 将 label 移到第 3 行
    ui->gridLayout->removeWidget(ui->label);
    ui->gridLayout->addWidget(ui->label, 3, 0, 1, 2);
}

void frmVideoWindow::onFenceToolClicked(int id)
{
    DrawMode mode = DrawMode::NoMode;
    if (id == 0) mode = DrawMode::RectDraw;
    else if (id == 1) mode = DrawMode::PolyDraw;
    else if (id == 2) mode = DrawMode::DeleteMode;

    PlayerWidget *pw = playerWidget(currentFenceChannel_);
    if (pw) {
        pw->fenceOverlay()->setDrawMode(mode);
    }
}

void frmVideoWindow::onFenceChannelClicked(int ch)
{
    // 取消上一个通道的绘制模式
    PlayerWidget *oldPw = playerWidget(currentFenceChannel_);
    if (oldPw && currentFenceChannel_ != ch) {
        oldPw->fenceOverlay()->setDrawMode(DrawMode::NoMode);
    }

    currentFenceChannel_ = ch;
    for (int i = 0; i < 4; ++i) {
        fenceChannelBtns_[i]->setChecked(i == ch);
    }

    // 保留绘制工具选中状态，让用户可以直接在新通道上画
    // 如果之前选了矩形/多边形/删除，切换通道后继续生效
    QAbstractButton *checkedDrawBtn = fenceDrawGroup_->checkedButton();
    if (checkedDrawBtn) {
        // 重新应用当前工具到新通道
        onFenceToolClicked(fenceDrawGroup_->id(checkedDrawBtn));
    }

    // 加载新通道的围栏数据并刷新显示
    PlayerWidget *pw = playerWidget(ch);
    if (pw) pw->fenceOverlay()->loadFences();
}
