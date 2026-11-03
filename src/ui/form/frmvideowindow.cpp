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
#include "theme.h"
#include <QTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QFrame>
#include <QIcon>
#include <QPixmap>
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
// 两个重载的关系：QString 版是"只有一个地址"的旧写法，包一条全默认的流再转给正式入口。
// 空地址一律不动现有画面 —— 半路清空配置时把 tile 打回"待接入"比黑屏更容易看懂。
// ============================================================================
void frmVideoWindow::openVideo(int ch, const QString &path)
{
    VideoSourceCfg source;
    source.url = path;
    openVideo(ch, source);
}

void frmVideoWindow::openVideo(int ch, const VideoSourceCfg &source)
{
    PlayerWidget *pw = playerWidget(ch);   // 根据通道号获取播放器指针
    if (pw && !source.url.isEmpty()) {     // 确保播放器有效且地址非空
        pw->open(source, ch);              // 地址与打开参数一起交给播放器
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
//
// 两种模式都要保持的约束：围栏工具栏占 (2, 0, 1, **2**) 即跨两列，
// 且 grid 的 columnStretch(0/1) 与 rowStretch(0/1) 都是 1。
// 放大时 player 占满 (0,0,2,2)、工具栏仍在第 2 行，跨列写法照样成立 ——
// 别把它拆回单列（那会让第 0 列被工具栏的 ~700px 最小宽度顶开，四格不再等大）。
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
 *   [通道:][1][2][3][4] | [角色:][■禁区][■岗区域] | [矩形][多边形][删除][清空]
 *
 * 位置：插入到主网格布局的第2行（视频画面下方，底部标签上方）
 *
 * 实现要点（代码动态创建，不在 .ui 中）：
 *   - 通道按钮 ×4 仅用于选中当前编辑通道，非互斥故 setExclusive(false)；
 *     矩形/多边形/删除为互斥绘制工具组（fenceDrawGroup_）；
 *     清空直接调 FenceManager::clearChannel + 刷新叠加层
 *   - 触屏档：工具栏总高 86px（按钮 72 + 边距 + 边框），字体跟全局正文 16px
 *   - 角色按钮自成一组（fenceRoleGroup_），且**带实色方块图标** ——
 *     触屏没有 hover，语义只写在 tooltip 里等于没写
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
    // 触屏档：按钮本体 72（min-height 54 + 2*8 padding + 2*1 border）
    //         + 上下布局边距 2*6 + 面板边框 2*1 = 86。
    // 这 50px 是从 4 路画面区里扣的，画围栏时唯一的操作面，值。
    toolbar->setFixedHeight(86);

    // 设置工具栏的暗色主题样式
    // 注意：这里的 QPushButton 内联规则特异性高于 blacksoft.css 的全局 T1，
    // 所以必须自带 min-height，否则整条工具栏的按钮不会跟着放大。
    // :disabled 那条是给「撤销顶点 / 闭合」用的：触屏没有 hover，也没有 tooltip，
    // 非多边形模式下点这两颗钮本来就不该有反应，灰态是唯一能告诉用户"这条现在不归你管"的手段。
    toolbar->setStyleSheet(
        QString("QFrame { background: %1; border: 1px solid %2; }"                 // 框架背景和边框
                "QPushButton { background: %2; color: %3; border: 1px solid %4;"   // 按钮默认样式
                "  border-radius: 4px; min-height:54px; padding: 8px 14px;"        // T1 命中区
                "  font-size:%5px; }"
                "QPushButton:hover { background: %4; }"                            // 鼠标悬停样式
                "QPushButton:checked { background: %6; color: %7; }"               // 选中状态样式（主题强调色）
                "QPushButton:disabled { background: %9; color: %10; border-color: %9; }"
                "QLabel { color: %8; font-size:%5px; }")                           // 标签文字样式
            .arg(theme::PANEL, theme::BORDER)
            .arg(theme::TEXT)
            .arg(theme::HOVER)
            .arg(theme::FS_BODY)
            .arg(theme::ACCENT)
            .arg(theme::BG)
            .arg(theme::TEXT_MUTED)
            .arg(theme::BG)          // %9 灰态底：比面板再暗一档，一眼看出"这颗现在不干活"
            .arg(theme::TEXT_MUTED)); // %10 灰态字

    // ========================================================================
    // 第二步：创建水平布局并设置间距
    // ========================================================================
    QHBoxLayout *lay = new QHBoxLayout(toolbar);  // 创建水平布局，父对象为按钮容器
    lay->setContentsMargins(6, 6, 6, 6);          // 布局边距 2→6，给 72px 按钮留出可点的呼吸
    lay->setSpacing(8);                           // 控件间距 4→8，避免相邻钮边缘粘连

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
        btn->setFixedSize(56, 72);         // 单字符"1"不需要宽，但要够高够点（8mm 档）
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
    // 第四点五步：选择角色（下一个画出来的形状触发哪种报警）
    // ========================================================================
    // 与"矩形/多边形/删除"是正交的两件事：工具决定怎么画，角色决定画出来的
    // 形状是"禁区（进去就报）"还是"岗区域（在外面就报）"。因此单独成组，
    // 默认选禁区 —— 与围栏机制早期"只有一种模式"的默认行为一致。
    lay->addWidget(new QLabel("角色:", toolbar));

    // 角色语义必须**看得见**：触屏没有 hover，写在 tooltip 里等于没写。
    // 所以按钮文字前挂一个与画布同色的实色方块（复用 FenceOverlay::roleColor，
    // 红=禁区、绿=岗区域），tooltip 降级为补充说明。
    // 宽度不再 setFixedSize：撞上新 padding(8x14) 会把"岗区域"三个字吃掉，
    // 只给 minimumHeight，让宽度跟着文字走 —— 这是"不截字"的直接手段。
    auto makeRoleSwatch = [](geofence::FenceRole role) {
        QPixmap pm(14, 14);
        pm.fill(geofence::FenceOverlay::roleColor(role));
        return QIcon(pm);
    };

    QPushButton *forbiddenBtn = new QPushButton("禁区", toolbar);
    forbiddenBtn->setObjectName("fenceRoleForbidden");
    forbiddenBtn->setCheckable(true);
    forbiddenBtn->setMinimumHeight(72);
    forbiddenBtn->setIcon(makeRoleSwatch(geofence::FenceRole::Forbidden));
    forbiddenBtn->setIconSize(QSize(14, 14));
    forbiddenBtn->setToolTip(QStringLiteral("画出的形状是禁区：目标进入即报警（红）"));
    lay->addWidget(forbiddenBtn);

    QPushButton *workzoneBtn = new QPushButton("岗区域", toolbar);
    workzoneBtn->setObjectName("fenceRoleWorkzone");
    workzoneBtn->setCheckable(true);
    workzoneBtn->setMinimumHeight(72);
    workzoneBtn->setIcon(makeRoleSwatch(geofence::FenceRole::Workzone));
    workzoneBtn->setIconSize(QSize(14, 14));
    workzoneBtn->setToolTip(QStringLiteral(
        "画出的形状是岗区域：目标出现在所有岗区域之外即报警（绿）。\n"
        "注意报的是\"人跑到区域外\"，不是\"区域里没人\"。"));
    lay->addWidget(workzoneBtn);

    // 角色按钮组（互斥，与绘制工具组分开；默认禁区）
    fenceRoleGroup_ = new QButtonGroup(this);
    fenceRoleGroup_->setExclusive(true);
    fenceRoleGroup_->addButton(forbiddenBtn, 0);
    fenceRoleGroup_->addButton(workzoneBtn, 1);
    forbiddenBtn->setChecked(true);

    connect(fenceRoleGroup_, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
            this, [this](QAbstractButton *) { applyFenceRole(currentFenceChannel_); });

    lay->addSpacing(10);
    lay->addWidget(new QLabel("|", toolbar));

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

    // ========================================================================
    // 第五步之二：多边形的两个动作钮（触屏唯一可靠的闭合入口）
    // ========================================================================
    // 闭合原本只有两条路：右键（fence_overlay.cpp mousePressEvent 的 RightButton 分支）
    // 和双击（mouseDoubleClickEvent）。电容屏上右键不存在，双击在按下-抬起的间隔稍长
    // 时被识别成拖动 —— 于是"多边形画得出来、合不上"。这两颗钮补的就是那一步。
    // 刻意不进 fenceDrawGroup_（那个组是互斥的**模式**，这里是**动作**），
    // 右键与双击路径原样保留，鼠标用户行为不变。
    fenceUndoPointBtn_ = new QPushButton("撤销顶点", toolbar);
    fenceUndoPointBtn_->setObjectName("fenceUndoPoint");
    lay->addWidget(fenceUndoPointBtn_);

    fenceClosePolyBtn_ = new QPushButton("闭合", toolbar);
    fenceClosePolyBtn_->setObjectName("fenceClosePoly");
    lay->addWidget(fenceClosePolyBtn_);

    // 初始是空闲模式（没有工具被选中），两颗动作钮先灰着，由 onFenceToolClicked 点亮。
    fenceUndoPointBtn_->setEnabled(false);
    fenceClosePolyBtn_->setEnabled(false);

    // 点钮时同时给"当前通道的那块画布"下指令；overlay 自己在闭合后清顶点，
    // 撤销只删最后一个点，两者的空态都是静默无操作。
    connect(fenceUndoPointBtn_, &QPushButton::clicked, this, [this]() {
        if (auto *pw = playerWidget(currentFenceChannel_))
            if (pw->fenceOverlay()) pw->fenceOverlay()->undoLastPoint();
    });
    connect(fenceClosePolyBtn_, &QPushButton::clicked, this, [this]() {
        if (auto *pw = playerWidget(currentFenceChannel_))
            if (pw->fenceOverlay()) pw->fenceOverlay()->closePolygon();
    });

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
    // 第九步：工具栏末尾加弹簧（居左），并把工具栏插进网格
    // ========================================================================
    // 弹簧必须在**工具栏自己的 QHBoxLayout** 里。上一版把它当 grid 的第 2 行第 1 列项
    // （addItem(spacer, 2, 1)）塞进网格，于是第二列只被 40px 的弹簧撑着，而工具栏
    // ~700px 的最小宽度全压在单一列上 —— 四宫格左右两列就此不等宽，这是
    // "四个监控不一样大"的唯一成因（四个 PlayerWidget 在 .ui 里是对称的）。
    // 居左效果一模一样，但不再往网格灌最小宽度。
    lay->addStretch();

    // 插入到 gridLayout 的第2行，跨2列（视频画面下方）。
    // 放大模式下 player 占 (0,0,2,2)、工具栏仍在第 2 行，跨列写法两种模式都成立。
    ui->gridLayout->addWidget(toolbar, 2, 0, 1, 2);

    // 显式声明"两列两行等分"。不依赖 QGridLayout 那条"stretch 全为 0 时把额外空间
    // 平分"的隐含行为 —— 一旦有人再往某列塞一个窄控件，隐含行为就静默变成不对称。
    ui->gridLayout->setColumnStretch(0, 1);
    ui->gridLayout->setColumnStretch(1, 1);
    ui->gridLayout->setRowStretch(0, 1);
    ui->gridLayout->setRowStretch(1, 1);
}

void frmVideoWindow::applyFenceRole(int channel)
{
    if (!fenceRoleGroup_) return;
    PlayerWidget *pw = playerWidget(channel);
    if (!pw || !pw->fenceOverlay()) return;

    // 未选中（理论上不会发生：互斥组默认选了一个）按禁区处理，与 FenceOverlay 的默认值一致
    const bool workzone = fenceRoleGroup_->checkedId() == 1;
    pw->fenceOverlay()->setRole(workzone ? geofence::FenceRole::Workzone
                                         : geofence::FenceRole::Forbidden);
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

    // 「撤销顶点 / 闭合」只在多边形模式下有意义：这两颗钮在矩形/删除模式下点下去
    // 什么也不会发生（overlay 的顶点列表本来就是空的），与其让用户按下一次空的，
    // 不如让它一开始就看着按不动。
    if (fenceUndoPointBtn_)  fenceUndoPointBtn_->setEnabled(mode == DrawMode::PolyDraw);
    if (fenceClosePolyBtn_)  fenceClosePolyBtn_->setEnabled(mode == DrawMode::PolyDraw);
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

    // 角色同理要跟着通道走：overlay 是每个通道各自的，不推过去的话
    // 新通道的 overlay 还停在它上一次的默认禁区，用户以为选的是岗区域。
    applyFenceRole(ch);

    // 加载新通道的围栏数据并刷新显示
    PlayerWidget *pw = playerWidget(ch);
    if (pw) pw->fenceOverlay()->loadFences();
}
