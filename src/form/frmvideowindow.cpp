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
    ui->gridLayout->setContentsMargins(0, 0, 0, 0);  // 去除 gridLayout 默认边距，消除左侧空白
    ui->gridLayout->setSpacing(0);                     // 去除视频窗口之间的间距
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
        qDebug() << str;
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
    
    QGridLayout* gridLayout = ui->gridLayout;
    
    if (expanded) {
        // 放大模式：隐藏其他通道，只显示选中通道，并占满整个网格
        expandedChannel_ = channel;
        
        for (int i = 0; i < 4; i++) {
            if (i == channel) {
                players[i]->show();
                players[i]->setExpanded(true);
                gridLayout->removeWidget(players[i]);
                gridLayout->addWidget(players[i], 0, 0, 2, 2);
            } else {
                players[i]->hide();
                gridLayout->removeWidget(players[i]);
            }
        }
    } else {
        // 恢复四宫格模式
        expandedChannel_ = -1;
        
        for (int i = 0; i < 4; i++) {
            players[i]->show();
            players[i]->setExpanded(false);
            gridLayout->removeWidget(players[i]);
        }
        gridLayout->addWidget(players[0], 0, 0);
        gridLayout->addWidget(players[1], 0, 1);
        gridLayout->addWidget(players[2], 1, 0);
        gridLayout->addWidget(players[3], 1, 1);
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

/**
 * @brief 设置电子围栏工具栏
 *
 * 功能：创建视频监控页面下方的电子围栏操作工具栏
 *       工具栏包含：通道选择按钮（1-4）、绘制工具（矩形/多边形/删除）、清空按钮
 *       用户可以通过工具栏选择通道并绘制电子围栏区域
 *
 * 布局结构：
 *   [通道:][1][2][3][4] | [矩形][多边形][删除][清空]
 *
 * 位置：插入到主网格布局的第2行（视频画面下方，底部标签上方）
 *
 * 实现要点（代码动态创建，不在 .ui 中）：
 *   - 通道按钮 ×4（28x24）仅用于选中当前编辑通道，非互斥故 setExclusive(false)；
 *     矩形/多边形/删除为互斥绘制工具组（fenceDrawGroup_）；
 *     清空直接调 FenceManager::clearChannel + 刷新叠加层
 *   - 工具栏高 36px、按钮 12px 字体为紧凑布局的经验尺寸
 *   - addWidget(toolbar, 2, 0, 1, 2) 占满两列，再把原第 2 行的提示 label
 *     挪到第 3 行，避免改动 .ui 文件
 */
void frmVideoWindow::setupFenceToolbar()
{
    // ========================================================================
    // 第一步：创建工具栏容器
    // ========================================================================
    QFrame *toolbar = new QFrame(this);           // 创建工具栏框架容器
    toolbar->setFrameStyle(QFrame::StyledPanel);  // 设置边框样式为带阴影的面板
    toolbar->setFixedHeight(36);                   // 固定工具栏高度为36像素

    // 设置工具栏的暗色主题样式
    toolbar->setStyleSheet(
        "QFrame { background: #2d2d3d; border: 1px solid #3d3d4d; }"        // 框架背景和边框
        "QPushButton { background: #3d3d4d; color: #E5E7EB; border: 1px solid #45455c;"  // 按钮默认样式
        "  border-radius: 4px; padding: 2px 8px; font-size:13px; }"    // 圆角、内边距、字体
        "QPushButton:hover { background: #45455c; }"                    // 鼠标悬停样式
        "QPushButton:checked { background: #4fc3f7; color: #1e1e2e; }"  // 选中状态样式（主题强调色）
        "QLabel { color: #9ca3af; font-size:13px; }"                   // 标签文字样式
    );

    // ========================================================================
    // 第二步：创建水平布局并设置间距
    // ========================================================================
    QHBoxLayout *lay = new QHBoxLayout(toolbar);  // 创建水平布局，父对象为工具栏
    lay->setContentsMargins(2, 2, 2, 2);          // 设置布局边距：左2、上2、右2、下2（左边留小空白）
    lay->setSpacing(4);                           // 设置控件间距为4像素

    // ========================================================================
    // 第三步：添加通道选择区域
    // ========================================================================
    lay->addWidget(new QLabel("通道:", toolbar));  // 添加"通道:"文字标签

    // 创建通道选择按钮组（非互斥，因为支持多选）
    fenceToolGroup_ = new QButtonGroup(this);
    fenceToolGroup_->setExclusive(false);  // 设置为非互斥模式

    // 循环创建4个通道选择按钮（1、2、3、4）
    for (int i = 0; i < 4; ++i) {
        QPushButton *btn = new QPushButton(QString::number(i + 1), toolbar);  // 创建按钮，显示通道号
        btn->setCheckable(true);           // 设置按钮可选中（开关样式）
        btn->setFixedSize(28, 24);         // 设置按钮固定尺寸
        if (i == 0) btn->setChecked(true); // 默认选中通道1
        fenceChannelBtns_[i] = btn;        // 保存按钮指针到数组
        fenceToolGroup_->addButton(btn, i); // 添加到按钮组，ID为i
        lay->addWidget(btn);               // 添加到布局
        // 连接点击信号到通道切换槽函数
        connect(btn, &QPushButton::clicked, this, [this, i]() { onFenceChannelClicked(i); });
    }

    // ========================================================================
    // 第四步：添加分隔符
    // ========================================================================
    lay->addSpacing(10);                    // 添加10像素间距作为分隔
    lay->addWidget(new QLabel("|", toolbar)); // 添加竖线分隔符

    // ========================================================================
    // 第五步：创建绘制工具按钮（矩形、多边形、删除）
    // ========================================================================
    // 矩形绘制按钮
    QPushButton *rectBtn = new QPushButton("矩形", toolbar);   // 创建"矩形"按钮
    rectBtn->setObjectName("fenceRect");                       // 设置对象名（用于样式识别）
    rectBtn->setCheckable(true);                               // 设置可选中
    lay->addWidget(rectBtn);                                   // 添加到布局

    // 多边形绘制按钮
    QPushButton *polyBtn = new QPushButton("多边形", toolbar); // 创建"多边形"按钮
    polyBtn->setObjectName("fencePoly");                       // 设置对象名
    polyBtn->setCheckable(true);                               // 设置可选中
    lay->addWidget(polyBtn);                                   // 添加到布局

    // 删除按钮
    QPushButton *deleteBtn = new QPushButton("删除", toolbar); // 创建"删除"按钮
    deleteBtn->setObjectName("fenceDelete");                   // 设置对象名
    deleteBtn->setCheckable(true);                             // 设置可选中
    lay->addWidget(deleteBtn);                                 // 添加到布局

    // ========================================================================
    // 第六步：创建清空按钮（非选中类型）
    // ========================================================================
    QPushButton *clearBtn = new QPushButton("清空", toolbar);  // 创建"清空"按钮
    clearBtn->setObjectName("fenceClear");                     // 设置对象名
    lay->addWidget(clearBtn);                                  // 添加到布局

    // ========================================================================
    // 第七步：创建绘制工具按钮组（互斥，同一时间只能选一个工具）
    // ========================================================================
    fenceDrawGroup_ = new QButtonGroup(this);   // 创建绘制工具按钮组
    fenceDrawGroup_->setExclusive(true);        // 设置为互斥模式（单选）
    fenceDrawGroup_->addButton(rectBtn, 0);     // 矩形按钮，ID=0
    fenceDrawGroup_->addButton(polyBtn, 1);     // 多边形按钮，ID=1
    fenceDrawGroup_->addButton(deleteBtn, 2);   // 删除按钮，ID=2

    // ========================================================================
    // 第八步：连接绘制工具按钮的点击信号
    // ========================================================================
    connect(rectBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(0); });    // 矩形
    connect(polyBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(1); });    // 多边形
    connect(deleteBtn, &QPushButton::clicked, this, [this]() { onFenceToolClicked(2); });  // 删除

    // 清空按钮：清空当前通道的所有围栏
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        // 调用FenceManager清空当前通道的围栏数据
        geofence::FenceManager::instance().clearChannel(currentFenceChannel_);
        // 刷新当前通道的围栏显示
        if (auto *pw = playerWidget(currentFenceChannel_))
            if (pw->fenceOverlay()) pw->fenceOverlay()->loadFences();
    });

    // ========================================================================
    // 第九步：将工具栏插入到主网格布局
    // ========================================================================
    // 插入到 gridLayout 的第2行，跨2列（视频画面下方）
    ui->gridLayout->addWidget(toolbar, 2, 0, 1, 1);  // 工具栏占第0列

    // 在右边添加弹簧控件，让工具栏居左显示
    QSpacerItem *spacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);
    ui->gridLayout->addItem(spacer, 2, 1, 1, 1);  // 弹簧占第1列
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
