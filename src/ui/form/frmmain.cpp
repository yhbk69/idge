#pragma execution_character_set("utf-8")

/**
 * @file frmmain.cpp
 * @brief 主窗口实现 - 施工行为监测与分析系统
 *
 * 核心功能：
 *   1. 界面初始化：无边框窗体、标题栏图标、顶部导航、左侧配置菜单
 *   2. 配置管理：从 config.json 读取所有配置，UI 修改后自动回写
 *   3. 视频控制：4路视频通道浏览、加载、实时切换
 *   4. 日志系统：带时间戳和颜色分类的日志输出（参考 rknn_Multithread）
 *
 * 配置统一管理（全部通过 ConfigManager 读写 config.json）：
 *   - video.channel1~4：4路视频源路径
 *   - model.path：RKNN 模型路径
 *   - model.label：标签文件路径
 *   - detect.conf_threshold：置信度阈值
 *   - detect.nms_threshold：NMS 阈值
 */

#include "frmmain.h"
#include "ui_frmmain.h"
#include "iconhelper.h"
#include "qthelper.h"
#include "theme.h"
#include "SharedTypes.hpp"
#include "ConfigManager.h"
#include "frmvideowindow.h"
#include "player_widget.h"
#include "dashboard_widget.h"
#include "alarm_list_widget.h"
#include "alarm_manager.h"
#include "video_alarm_widget.h"
#include "fence_manager.h"
#include "ws_client.h"
#include "alarm_uplink.h"
#include "http_file_service.h"   // 报警媒体的内嵌文件服务（url 模式；装配点在此）
#include "roll_call_widget.h"
#include "equipment_inventory_widget.h"
#include "smart_recognition_widget.h"
#include "login_widget.h"        // 扫码登录页（stackedWidget 第 0 页 = 门）
#include "construction_session.h"
#include "construction_detail_dialog.h"  // 点右上角登录人弹出的施工详情
#include "roll_call_service.h"
#include "equipment_inventory_service.h"
#include "model_registry.h"
#include "runtime_paths.h"
#include "easy_timer.h"  // yolo11_model.hpp 依赖 TIMER（与 ppe_task 同序）
#include <QScrollBar>   // 日志区"只在贴底时跟随"要读滚动条位置
#include "yolo11_model.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QMessageBox>
#include <QPushButton>
#include <QGuiApplication>
#include <QScreen>
#include <QLineEdit>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QApplication>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QScrollArea>
#include <QCheckBox>
#include <QListWidget>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QTableWidget>
#include <QProcess>
#include <QComboBox>

// ==========================================
// 构造 / 析构
// ==========================================

frmMain::frmMain(QWidget *parent) : QWidget(parent), ui(new Ui::frmMain)
{
    ui->setupUi(this);
    this->initForm();
    this->initStyle();
    this->initNewPages();
    this->initBusinessPages();
    this->initLeftMain();
    // 触屏拖动滚动挂到本页所有可滚动区。必须晚于**所有**页面构建：
    // findChildren 扫的是"此刻已存在"的子件，而 initForm() 之后还有
    // initNewPages/initBusinessPages/initLeftMain 在造件 —— 放 initForm 末尾会漏掉大半。
    // 按需 new 的对话框（通道模型选择框、智能识别）不在这里，各自在创建处补一行。
    //
    // 原先这里的最后一行是 on_btnMenu_Max_clicked()，作用只是把窗口摆成
    // availableGeometry()；全屏改由 main() 的 showFullScreen() 负责后它就成了冲突项
    // （构造里改 geometry 会打断全屏），见 Lane E 的说明。
    QtHelper::enableTouchScrollAll(this);

    // 触屏命中区审计：IDGE_TOUCHDUMP=1 时每 2s 打一遍**当前可见**的可点件真实高度。
    // 为什么必须实测而不能推算：min-height 不含 padding/border、setFixedSize 会封顶、
    // 内联 QSS 会打穿皮肤层，三条规则叠加后代码里的数字和屏幕上的像素经常不符。
    // 不开这个变量时零开销（定时器都不建）。
    if (qEnvironmentVariableIsSet("IDGE_TOUCHDUMP")) {
        QTimer *dumpTimer = new QTimer(this);
        dumpTimer->setInterval(2000);
        connect(dumpTimer, &QTimer::timeout, this, [this]() {
            QtHelper::dumpTouchGeometry(this);
        });
        dumpTimer->start();
    }

    // 门在最上面：所有页面都建好了才合门，这样登录页是构造完成后唯一可见的页面。
    // 四路视频此刻还没开（initForm 第 11 步已挪到 onLoginSucceeded），
    // 所以登录期间不推流、不检测、不出报警。
    showLoginGate();
}

frmMain::~frmMain()
{
    delete ui;
}

// ==========================================
// QSS 颜色提取工具函数
// ==========================================

/**
 * @brief 从 QSS 样式表中提取单个颜色值
 * @param qss   完整的 QSS 样式表字符串
 * @param flag  颜色标记（如 "TextColor:"）
 * @param color [out] 提取到的颜色值
 */
void frmMain::getQssColor(const QString &qss, const QString &flag, QString &color)
{
    int index = qss.indexOf(flag);
    if (index >= 0) {
        color = qss.mid(index + flag.length(), 7);
    }
}

/**
 * @brief 从 QSS 样式表中提取所有主题颜色
 * 包括：文字色、面板色、边框色、正常渐变色、深色渐变色、高亮色
 */
void frmMain::getQssColor(const QString &qss, QString &textColor, QString &panelColor,
                          QString &borderColor, QString &normalColorStart, QString &normalColorEnd,
                          QString &darkColorStart, QString &darkColorEnd, QString &highColor)
{
    getQssColor(qss, "TextColor:", textColor);
    getQssColor(qss, "PanelColor:", panelColor);
    getQssColor(qss, "BorderColor:", borderColor);
    getQssColor(qss, "NormalColorStart:", normalColorStart);
    getQssColor(qss, "NormalColorEnd:", normalColorEnd);
    getQssColor(qss, "DarkColorStart:", darkColorStart);
    getQssColor(qss, "DarkColorEnd:", darkColorEnd);
    getQssColor(qss, "HighColor:", highColor);
}

// ==========================================
// 日志功能（参考 rknn_Multithread 项目）
// ==========================================

/**
 * @brief 获取当前时间戳字符串
 * @return 格式: "yyyy-MM-dd HH:mm:ss"
 */
QString frmMain::currentTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

/**
 * @brief 添加日志消息（根据类别自动选择颜色）
 *
 * 类别颜色映射：
 *   - alarm / error  -> 红色 (220, 50, 50)
 *   - warning / warn -> 黄色 (220, 180, 0)
 *   - system         -> 蓝色 (50, 120, 220)
 *   - info           -> 绿色 (50, 180, 50)
 *   - 其他           -> 灰色 (180, 180, 180)
 *
 * @param category 日志类别
 * @param message  日志消息
 */
void frmMain::log(const QString &category, const QString &message)
{
    QColor color;
    QString cat_lower = category.toLower();

    if (cat_lower == "alarm" || cat_lower == "error") {
        color = QColor(220, 50, 50);    // 红色 - 报警/错误
    } else if (cat_lower == "warning" || cat_lower == "warn") {
        color = QColor(220, 180, 0);    // 黄色 - 警告
    } else if (cat_lower == "system") {
        color = QColor(50, 120, 220);   // 蓝色 - 系统操作
    } else if (cat_lower == "info") {
        color = QColor(50, 180, 50);    // 绿色 - 信息
    } else if (cat_lower == "fence") {
        color = QColor(200, 120, 255);  // 紫色 - 围栏检测
    } else {
        color = QColor(180, 180, 180);  // 灰色 - 默认
    }

    logWithColor(category, message, color);
}

/**
 * @brief 添加带颜色的日志消息到 QTextBrowser
 *
 * 日志格式: [yyyy-MM-dd HH:mm:ss][类别] 消息内容
 * - 时间戳：灰色
 * - 类别：对应颜色 + 加粗
 * - 消息：对应颜色
 *
 * @param category 日志类别
 * @param message  日志消息
 * @param color    文字颜色
 */
void frmMain::logWithColor(const QString &category, const QString &message, const QColor &color)
{
    // 先记下"用户此刻是不是贴着底部"。这条判断是日志区能用手划的前提：
    // 围栏状态日志大约每秒一条，原来无条件 ensureCursorVisible() 等于每次追加都把
    // 视图拽回底部 —— 手指刚划上去，下一条日志就把它弹回来，看着像"日志划不动"。
    // 留 4px 容差：末行行高与视口不可能像素级相等，惯性动画收尾也会差零点几。
    QScrollBar *bar = ui->textBrowserLog->verticalScrollBar();
    const bool followTail = !bar || bar->value() >= bar->maximum() - 4;

    QTextCursor cursor = ui->textBrowserLog->textCursor();
    cursor.movePosition(QTextCursor::End);

    // 时间戳（灰色）
    QTextCharFormat tsFormat;
    tsFormat.setForeground(QColor(128, 128, 128));
    cursor.insertText(QString("[%1]").arg(currentTimestamp()), tsFormat);

    // 类别（带颜色，加粗）
    QTextCharFormat catFormat;
    catFormat.setForeground(color);
    catFormat.setFontWeight(QFont::Bold);
    cursor.insertText(QString("[%1] ").arg(category), catFormat);

    // 消息内容（带颜色）
    QTextCharFormat msgFormat;
    msgFormat.setForeground(color);
    cursor.insertText(message + "\n", msgFormat);

    // 自动滚动到底部 —— 仅在此前就贴底时。不贴底时连 setTextCursor 都不做：
    // 它会顺带滚动视图，等于把"用户在往上翻"这件事又抹掉。
    if (followTail) {
        ui->textBrowserLog->setTextCursor(cursor);
        ui->textBrowserLog->ensureCursorVisible();
    }
}

// ==========================================
// 窗体初始化
// ==========================================

/**
 * @brief 初始化窗体基础属性
 *
 * 执行顺序：
 *   1. 设置无边框窗体
 *   2. 设置标题栏图标（FontAwesome）
 *   3. 设置标题文字
 *   4. 配置顶部导航按钮
 *   5. 初始化左侧导航
 *   6. 加载 config.json 并初始化调试页面
 */
void frmMain::initForm()
{
    // 1. 设置无边框窗体
    QtHelper::setFramelessForm(this);

    // 2. 设置标题栏图标
    IconHelper::setIcon(ui->btnMenu_Min, 0xf068);       // 最小化
    IconHelper::setIcon(ui->btnMenu_Max, 0xf067);       // 最大化
    IconHelper::setIcon(ui->btnMenu_Close, 0xf00d);     // 关闭
    // 标题左侧那个日历 glyph 与"施工行为监测与分析"没有语义关系（模板遗留），
    // 触屏上去掉它可以给标题与导航行多留横向空间。三枚窗口钮的 glyph 保留 ——
    // 去掉就没有窗口控制入口了。
    ui->labIco->hide();

    // 3. 标题栏属性
    ui->widgetTitle->setProperty("form", "title");
    // 原来这里 installEventFilter 拦标题栏双击切最大化 —— 触屏上那是误触源：
    // 一旦误触，围栏 overlay 的像素坐标会随窗口尺寸漂移，直接变成漏报/误报。
    // 拖动移动窗口由 QtHelper::setFramelessForm 负责，不经过这个过滤器。
    ui->widgetTop->setProperty("nav", "top");

    // 4. 设置标题文字
    QFont font;
    font.setPixelSize(18);
    font.setBold(true);
    ui->labTitle->setFont(font);
    ui->labTitle->setText("施工行为监测与分析");
    this->setWindowTitle(ui->labTitle->text());

    // 5. stackedWidget 全局样式（统一基础字号，页面内可再覆盖）
    ui->stackedWidget->setStyleSheet(QString("QLabel{font-size:%1px;}").arg(theme::FS_BODY));

    // 6. 配置顶部导航按钮
    // 触屏改造：去掉图标。图标占掉 22px 横向 + 让"图上字下"叠成两行，
    // 是导航字号不敢往上调的直接原因。只剩文字后横向腾出空间，字号才敢 14→18。
    // 竖向 padding 由 blacksoft.css 的 QWidget[nav="top"] QAbstractButton 给（8px），
    // 所以 min-height 54 + 2*8 = 72px 总高，正好是 T1 档。
    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        QToolButton *tbtn = qobject_cast<QToolButton *>(btn);
        btn->setIcon(QIcon());
        btn->setMinimumWidth(112);
        btn->setCheckable(true);
        if (tbtn) {
            tbtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
            tbtn->setMinimumHeight(theme::TOUCH_MAIN_MIN_H);
            QFont bf = tbtn->font();
            bf.setPixelSize(theme::FS_NAV_TOUCH);
            tbtn->setFont(bf);
        }
        connect(btn, SIGNAL(clicked()), this, SLOT(buttonClick()));
    }

    // 标题栏三钮区在 .ui 里被锁成 100px 宽（每人 33px，鼠标够用、手指不够）。
    // .ui 不手改（见 src/ui/form/README.md 的既有约定），这里用代码覆盖上下限。
    ui->widgetMenu->setMinimumWidth(160);
    ui->widgetMenu->setMaximumWidth(160);

    // 6.5 登录人角标：插在 spacer 与三钮区之间 ⇒ 贴着标题栏右上角，未登录时整颗隐藏。
    //     它是 widgetTitle 的直接子件、不在 widgetTop 里，所以第 6 步那句
    //     findChildren<QAbstractButton*> 的导航批量设置和 buttonClick() 的文字分派
    //     都不会碰到它 —— 挂进 widgetTop 反而会被当成导航项去切页面。
    sessionBadge_ = new QPushButton(ui->widgetTitle);
    sessionBadge_->setCursor(Qt::PointingHandCursor);
    sessionBadge_->setMinimumHeight(theme::TOUCH_MAIN_MIN_H);
    sessionBadge_->setMaximumHeight(theme::TOUCH_MAIN_MIN_H);
    sessionBadge_->setStyleSheet(
        QString("QPushButton { color:%1; background:%2; border:1px solid %3; border-radius:6px;"
                "padding:0 16px; font-size:%4px; }"
                "QPushButton:hover { background:%5; }"
                "QPushButton:pressed { background:%6; }")
            .arg(theme::TEXT, theme::FIELD, theme::BORDER)
            .arg(theme::FS_NAV_TOUCH)
            .arg(theme::HOVER, theme::PANEL));
    connect(sessionBadge_, &QPushButton::clicked, this, &frmMain::openSessionDetail);
    sessionBadge_->hide();
    // 落在"倒数第一项"= 三钮区 widgetMenu 之前，比写死下标 4 稳：`.ui` 万一在标题栏
    // 加装东西，这里也不会把角标插到错误位置。
    ui->horizontalLayout_2->insertWidget(ui->horizontalLayout_2->count() - 1, sessionBadge_);

    // 默认选中"视频监控"
    ui->btnMain->click();

    // 7. 左侧导航按钮样式
    ui->widgetLeftMain->setProperty("flag", "left");
    ui->pageMonitor->setStyleSheet(QString("QWidget[flag=\"left\"] QAbstractButton{min-height:%1px;max-height:%1px;}").arg(60));

    // 8. 获取视频监控窗口指针（用于后续打开视频）
    videoWindow = ui->lab1;

    // 9. 加载配置文件（内部会把老 schema 一次性迁移到按通道配置）
    ConfigManager::instance().load(RuntimePaths::configFile());

    // 10. 设置各通道围栏覆盖层的通道号 + 加载围栏配置
    //     必须在开流之前：解码器建推理任务时要检查"本通道有没有模型能产出围栏类别"，
    //     晚加载会让这条告警静默跳过（围栏本身在 decodeLoop 里逐帧读，不受时序影响）。
    //     ⚠ 这两步的先后**不能换**：setChannel() 内部会立刻 loadFences()，
    //     而 loadFences() 读的是 FenceManager 的内存表。先 setChannel 就等于让四块画布
    //     各自去抄一份还没灌过的空表 —— 结果开机后一条围栏线都不画（报警照报，因为
    //     判定端逐帧现读），要等用户点一下工具栏的通道钮才"突然"显示出来。
    //     2026-11-01 板端实测：重启后四宫格无任何围栏线，点通道2→通道1 后红框立刻出现。
    geofence::FenceManager::instance().loadFromConfig();
    for (int i = 0; i < 4; ++i) {
        PlayerWidget *pw = videoWindow->playerWidget(i);
        if (pw) pw->setChannel(i);
    }

    // 11. 四路视频**不在这里开**。扫码登录前不推流、不检测、不出报警，
    //     开流动作在 onLoginSucceeded() 里经 startAllStreams() 完成（幂等）。
    //     enabled=false 的通道留空，四宫格显示"未开流"。运行中改这个开关（以及
    //     模型列表）不需要重启：设置页会经 applyChannelConfigNow() 重开该路。

    // 12. 初始化调试帮助页（从 config 读取配置填充 UI）
    initDebugPage();
}

/**
 * @brief 加载 QSS 样式表并提取主题颜色
 * 样式表文件: :/qss/blacksoft.css（黑色主题）
 */
void frmMain::initStyle()
{
    QString qss = QtHelper::getStyle(":/qss/blacksoft.css");
    if (!qss.isEmpty()) {
        QString paletteColor = qss.mid(20, 7);
        qApp->setPalette(QPalette(QColor(paletteColor)));
        qApp->setStyleSheet(qss);
    }

    // 从样式表中提取主题颜色
    QString textColor, panelColor, borderColor, normalColorStart, normalColorEnd, darkColorStart, darkColorEnd, highColor;
    getQssColor(qss, textColor, panelColor, borderColor, normalColorStart, normalColorEnd, darkColorStart, darkColorEnd, highColor);

    // 保存颜色到成员变量（供导航栏使用）
    this->borderColor = highColor;
    this->normalBgColor = normalColorStart;
    this->darkBgColor = panelColor;
    this->normalTextColor = textColor;
    this->darkTextColor = normalTextColor;
}

void frmMain::initNewPages()
{
    // 跨线程队列连接需要注册元类型
    qRegisterMetaType<object_detect_result_list>("object_detect_result_list");
    qRegisterMetaType<AlarmRecord>("AlarmRecord");

    // ===== 顶部导航：视频监控 / 数据看板 / 报警数据 / 调试设置 / 退出 =====
    // （人员点名、设备盘点、智能识别三项在下方及 initBusinessPages() 补齐，共 8 项）
    // 只设标签、不设图标：initForm() 已把导航钮切成 ToolButtonTextOnly 并清空 icon，
    // 触屏档下图标不显示（.ui 里遗留的原始语义图标也一并作废）。
    ui->btnMain->setText("视频监控");
    ui->btnRoll->setText("数据看板");
    ui->btnEquipCheck->setText("报警数据");
    ui->btnHelp->setText("调试设置");
    // 「退出」改成「退出登录」：进程不死，只回到扫码页并停流。
    // 真正的退出程序入口在登录页那颗按钮与标题栏 ×（buttonClick 里按对象身份分派，
    // 不再按文字 —— 改文字不改分派会让这颗钮静默失效）。
    ui->btnExit->setText("退出登录");
    // 隐藏多余的占位按钮（人员点名/设备盘点原义、报警查询、系统设置）
    // btnSmart（智能识别）的标签由 .ui 直接提供，无需在此处理
    ui->btnData->hide();
    ui->btnConfig->hide();

    // 向 stackedWidget 添加数据看板和报警记录页面
    dashboardWidget_ = new DashboardWidget();
    alarmListWidget_ = new AlarmListWidget();

    // 插入到 pageMonitor(视频监控) 之后：index 1=数据看板, 2=报警数据
    ui->stackedWidget->insertWidget(1, dashboardWidget_);
    ui->stackedWidget->insertWidget(2, alarmListWidget_);

    // ===== 扫码登录页：stackedWidget 的第 0 页（门）=====
    // 代码里 insertWidget，不碰 .ui（src/ui/form/README.md 的既有约定）。
    // 页面切换全按控件指针（setCurrentWidget），所以插到 0 不会让别处的索引漂移。
    login_ = new LoginWidget();
    login_->setStyleSheet(QString("background: %1;").arg(theme::BG));
    ui->stackedWidget->insertWidget(0, login_);
    connect(login_, &LoginWidget::loginSucceeded, this, &frmMain::onLoginSucceeded);
    connect(login_, &LoginWidget::loginFailed, this, [this](const QString &reason) {
        appendLog(QString::fromUtf8("登录失败：%1").arg(reason));
    });
    connect(login_, &LoginWidget::exitProgramRequested, this, &frmMain::systemExit);

    // 设置背景
    dashboardWidget_->setStyleSheet(QString("background: %1;").arg(theme::BG));
    alarmListWidget_->setStyleSheet(QString("background: %1;").arg(theme::BG));

    // ===== 视频监控页面右侧报警列表 =====
    if (ui->widgetRightMain) {
        videoAlarmWidget_ = new VideoAlarmWidget(ui->widgetRightMain);
        QVBoxLayout *rightLayout = new QVBoxLayout(ui->widgetRightMain);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->addWidget(videoAlarmWidget_);

        // 连接报警更新信号
        connect(videoAlarmWidget_, &VideoAlarmWidget::alarmUpdated, this, [this]() {
            alarmListWidget_->refreshTable();
            updateAlarmBadge();
        });

        // 连接新报警信号：报警在 4 路通道上高频触发（person 每几帧一条），
        // 若每条都同步重建视频页报警面板（delete+重建 20 个 widget + 遍历
        // 全部 703 条内存记录），GUI 线程 event loop 会被彻底拖垮——实测表现
        // 为解码已稳在 30fps、paintGL 本身仅 0.7ms，但 onFrameReady/paintGL
        // 的"到达/绘制"仍被挤压成 2↔56 的抖动。这里改成节流：一个短窗内多次
        // 报警只合并重建一次面板，把 event loop 还给视频渲染。
        connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
                this, [this](const AlarmRecord &) {
                    if (videoAlarmWidget_ && ui->stackedWidget->currentWidget() == ui->pageMonitor) {
                        if (!alarmRefreshTimer_->isActive())
                            alarmRefreshTimer_->start();
                    }
                });
    }

    // ===== 悬浮报警提示 toast =====
    alarmToast_ = new QLabel(this);
    alarmToast_->setStyleSheet(
        QString("QLabel { background: rgba(220,50,50,0.92); color: white;"
                " border-radius: 6px; padding: 10px 16px; font-size: %1px; font-weight: bold; }")
            .arg(theme::FS_BODY));
    alarmToast_->setVisible(false);
    alarmToast_->setWordWrap(true);
    alarmToast_->adjustSize();
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, alarmToast_, &QLabel::hide);

    // 报警面板节流定时器：单次触发 400ms。窗口内首个报警启动它，期间再来的
    // 报警被忽略（timer 已 active），到期只重建一次面板 → 最多 2.5 次/秒，
    // 而不是每帧一次。
    alarmRefreshTimer_ = new QTimer(this);
    alarmRefreshTimer_->setSingleShot(true);
    alarmRefreshTimer_->setInterval(400);
    connect(alarmRefreshTimer_, &QTimer::timeout, this, [this]() {
        if (videoAlarmWidget_) videoAlarmWidget_->refreshAlarms();
    });

    // ===== 未确认报警角标 =====
    // 导航钮去掉图标后文字矩形上移，角标必须跟着重算，否则数字压字。
    // 尺寸从 15/12px 抬到 24/13px：触屏上这枚徽标也顺带要看得清。
    alarmBadge_ = new QLabel(ui->btnEquipCheck);
    alarmBadge_->setStyleSheet(
        QString("QLabel { background: %1; color: white; border-radius: 12px;"
                " font-size: %2px; font-weight: bold; min-width: 24px; max-width: 48px;"
                " padding: 3px 6px; }")
            .arg(theme::DANGER)
            .arg(theme::FS_HINT));
    alarmBadge_->setAlignment(Qt::AlignCenter);
    alarmBadge_->setVisible(false);
    alarmBadge_->raise();

    // 连接所有 4 路解码器的检测结果信号到 AlarmManager
    for (int ch = 0; ch < 4; ch++) {
        PlayerWidget *pw = videoWindow->playerWidget(ch);
        if (pw && pw->decoder()) {
            connect(pw->decoder(), &FFmpegVideoDecoder::statusChanged,
                    this, [this](int ch, int online) {
                        AlarmManager::instance().setChannelOnline(ch, online != 0);
                    });
        }
    }

    // 报警信号 -> 悬浮提示 / 角标刷新
    connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
            this, &frmMain::showAlarmToast);
    connect(&AlarmManager::instance(), &AlarmManager::statsUpdated,
            this, &frmMain::updateAlarmBadge);

    // 围栏日志信号 -> 调试页面日志
    connect(&geofence::FenceManager::instance(), &geofence::FenceManager::logMessage,
            this, [this](const QString &cat, const QString &msg) {
                log(cat, msg);
            });

    // WebSocket 客户端
    connect(&WsClient::instance(), &WsClient::logMessage,
            this, [this](const QString &cat, const QString &msg) {
                log(cat, msg);
            });
    connect(&WsClient::instance(), &WsClient::alarmReceived,
            this, &frmMain::onWsAlarmReceived);
    connect(&WsClient::instance(), &WsClient::connected,
            this, [this]() { log("system", "WebSocket 已连接到 Aibox"); });
    WsClient::instance().start();

    // 报警上行（P0-1）：本机生成的报警两段式发往对端，出厂默认关闭
    connect(&AlarmUplink::instance(), &AlarmUplink::logMessage,
            this, [this](const QString &cat, const QString &msg) {
                log(cat, msg);
            });

    // url 模式（默认）要先把本机文件服务起起来，JSON 里才可能有能下载的地址。
    // 起在这里而不是 AlarmUplink 内部：依赖方向是 base ← media ← biz ← ui，biz 反过来
    // include media 域的头就是破例，所以由装配层起服务、拿底址、注进去。
    // ⚠ stop 在 src/main.cpp 的事件循环之后，与这里故意不成对：mk_env_init 是进程级
    //   一次、停了不能再起，生命周期必须覆盖整个进程（见 http_file_service.h 文件头）。
    ConfigManager &uplinkCfg = ConfigManager::instance();
    if (uplinkCfg.alarmUplinkEnabled()
        && uplinkCfg.alarmUplinkMediaTransport() == QLatin1String(uplink::kTransportUrl)) {
        HttpFileService &svc = HttpFileService::instance();
        // alarmsDir() 是随 cwd 变的相对路径，服务要的是绝对路径：给错了症状是
        // "URL 全 404，但本地文件明明在"（它退回库默认的 <exe 目录>/www 当根）
        const QString root = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
        const quint16 port = static_cast<quint16>(uplinkCfg.alarmUplinkMediaHttpPort());
        if (!svc.start(root, port)) {
            log("warning", QStringLiteral("报警媒体文件服务没起来 port=%1，"
                                          "本次不申报媒体地址").arg(port));
        }
        // 地址前缀：配置写了就用配置的（服务没起来也照用，可能指向别处的反代）
        AlarmUplink::instance().setMediaBaseUrl(svc.baseUrl(uplinkCfg.alarmUplinkMediaBaseUrl()));
    }

    AlarmUplink::instance().attach();

    // 类别名表由解码器按通道槽位建立（见 FFmpegVideoDecoder::buildCascadeTasks），
    // 这里不再从全局标签文件填 AlarmManager。

    updateAlarmBadge();
}

void frmMain::showAlarmToast(const AlarmRecord &alarm)
{
    if (!alarmToast_) return;

    QDateTime dt = QDateTime::fromString(alarm.alarmTime, Qt::ISODate);
    // 围栏事件与类别事件分开措辞：都写成"检测到 person"会让人以为围栏通道上的
    // COCO 在按类别报警，而那个模型的 alarm_class 明明为空（只画框不报警）。
    // 围栏内部再按角色分两种（闯入禁区 / 离岗），弹一条"围栏侵入"却在人跑出去时报，
    // 处置的人就会往反方向找。
    QString head;
    if (alarm.isFenceAlarm) {
        head = alarm.alarmType == QLatin1String("fence_out")
            ? QString("⚠ 通道%1 离岗（在岗区域外） %2").arg(alarm.channel + 1).arg(alarm.className)
            : QString("⚠ 通道%1 闯入禁区 %2").arg(alarm.channel + 1).arg(alarm.className);
    } else {
        head = QString("⚠ 通道%1 检测到 %2").arg(alarm.channel + 1).arg(alarm.className);
    }
    // 负责人挂在时间后面；为空就不拼（toast 是右下角那一块，多一段空格比少一段更难看，
    // 而未登录期间根本不会有报警，这条只在旧历史回读之类的场景才空）
    const QString worker = alarm.workerName.isEmpty()
        ? QString()
        : QString("  负责人 %1").arg(alarm.workerName);
    alarmToast_->setText(QString("%1 (%2%%)  %3%4")
        .arg(head)
        .arg(alarm.confidence * 100, 0, 'f', 1)
        .arg(dt.toString("HH:mm:ss"), worker));
    alarmToast_->adjustSize();
    // 右下角悬浮显示
    int x = this->width() - alarmToast_->width() - 20;
    int y = this->height() - alarmToast_->height() - 20;
    alarmToast_->move(x > 0 ? x : 0, y > 0 ? y : 0);
    alarmToast_->show();
    alarmToast_->raise();
    toastTimer_->start(3000);

    // 新报警到达时若正在看报警页也刷新（列表内部已自行刷新）
    updateAlarmBadge();
}

void frmMain::updateAlarmBadge()
{
    if (!alarmBadge_) return;
    int unack = AlarmManager::instance().unacknowledgedCount();
    if (unack > 0) {
        alarmBadge_->setText(QString::number(unack > 99 ? 99 : unack));
        alarmBadge_->adjustSize();
        // 定位到按钮右上角。y=0 不是随手写的：导航钮 72 高、文字居中占 24~48，
        // 角标高 24 贴在 0~24 正好与文字上下相切，压不到"报警数据"四个字。
        int bw = ui->btnEquipCheck->width();
        alarmBadge_->move(bw - alarmBadge_->width() - 4, 0);
        alarmBadge_->show();
        alarmBadge_->raise();
    } else {
        alarmBadge_->hide();
    }
}

void frmMain::onWsAlarmReceived(const QJsonObject &data)
{
    QString alarmId = data["alarm_id"].toString();
    QString alarmType = data["alarm_type"].toString();
    long timestamp = static_cast<long>(data["timestamp"].toDouble());
    QString imageUrl = data["image_url"].toString();
    QString videoUrl = data["video_url"].toString();

    log("alarm", QString("WebSocket 报警: %1 (ID: %2)").arg(alarmType).arg(alarmId));

    if (!imageUrl.isEmpty()) {
        log("info", QString("  截图: %1").arg(imageUrl));
    }
    if (!videoUrl.isEmpty()) {
        log("info", QString("  视频: %1").arg(videoUrl));
    }

    // 发送 ack 确认
    if (!alarmId.isEmpty()) {
        WsClient::instance().sendAck(alarmId);
    }

    Q_UNUSED(timestamp);
}

// ==========================================
// 人员点名 / 设备盘点业务模块（caichao 分支合并）
// ==========================================

/**
 * @brief 初始化人员点名/设备盘点模块
 * 两个业务页不再占用顶部导航：页面装进各自的 QDialog（窗口级模态），
 * 由智能识别页的"人员点名"/"设备盘点"卡片通过 rollCallRequested /
 * equipmentRequested 信号拉起，入口收敛到智能识别 tab 的四张卡片。
 *
 * 初始化顺序是硬约束：
 *   1. 必须先 initRollCallService()——EquipmentInventoryService 的构造函数
 *      以 RollCallService 为依赖（两者共用 roll_call.db 的同一张 task 表，
 *      盘点任务也登记在该表，type="equipment_registration"）；
 *   2. 点名服务初始化失败只弹 warning、不中断启动：RollCallWidget 收到空的
 *      shared_ptr 后所有取数路径早退（service_ 判空），页面降级为空白列表，
 *      保证主监控功能不受业务模块拖累；失败同时把对应卡片置灰（构造参数
 *      rollCallAvailable/equipmentAvailable），避免用户点进一个空弹窗；
 *   3. 对话框用窗口级模态（WindowModal）而非 exec() 的应用级模态：点名详情
 *      是非模态顶层窗口（RollCallWidget::showTaskDetailDialog），应用级模态
 *      会把它一起锁死。窗口级模态只锁 frmMain，顶层详情窗照常可操作。
 * 顶部导航的 btnData/btnConfig 是 main 分支收敛后隐藏的占位按钮，这里不再
 * 复用（保持隐藏），.ui 中的 pageRoll 也不再挂载页面。
 */
void frmMain::initBusinessPages()
{
    QString ws = qEnvironmentVariable("IDGE_WORKSPACE");
    if (ws.isEmpty()) ws = QDir::currentPath();
    workspace_ = ws.toStdString();

    const bool rollCallOk = initRollCallService();
    if (!rollCallOk) {
        QMessageBox::warning(this, "警告", "人员点名服务初始化失败");
    }

    // 弹窗自带的关闭钮：这两个是 WindowModal，原本唯一出口是 mutter 服务端装饰的 ×
    // （约 24~28px，不是 QSS 能管的尺寸，皮肤层再怎么抬也抬不到它）。触屏上按不到
    // 就等于把人锁在模态窗里，只能回主界面重开程序。给一颗 T1（72px）兜底。
    auto addCloseBar = [](QDialog *dlg, QVBoxLayout *lay) {
        QHBoxLayout *bar = new QHBoxLayout();
        bar->setContentsMargins(16, 4, 16, 12);
        bar->addStretch();
        QPushButton *close = new QPushButton(QStringLiteral("关闭"));
        close->setMinimumWidth(120);
        close->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        bar->addWidget(close);
        lay->addLayout(bar);
        QObject::connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    };

    rollCallWidget_ = new RollCallWidget();
    rollCallWidget_->setService(rollCallService_);
    rollCallDialog_ = new QDialog(this);
    rollCallDialog_->setWindowTitle(QStringLiteral("人员点名"));
    rollCallDialog_->resize(1400, 860);
    rollCallDialog_->setWindowModality(Qt::WindowModal);
    rollCallDialog_->setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));
    auto* rollLayout = new QVBoxLayout(rollCallDialog_);
    rollLayout->setContentsMargins(0, 0, 0, 0);
    rollLayout->addWidget(rollCallWidget_);
    addCloseBar(rollCallDialog_, rollLayout);

    // 设备盘点（初始化失败与点名同级弹窗提示）
    const bool equipmentOk = initEquipmentService();
    if (!equipmentOk) {
        QMessageBox::warning(this, "警告",
                             "设备盘点服务初始化失败：模型库中未找到可用检测模型");
    }
    equipmentWidget_ = new EquipmentInventoryWidget();
    equipmentWidget_->setServices(equipmentService_, rollCallService_);
    equipmentDialog_ = new QDialog(this);
    equipmentDialog_->setWindowTitle(QStringLiteral("设备盘点"));
    equipmentDialog_->resize(1400, 860);
    equipmentDialog_->setWindowModality(Qt::WindowModal);
    equipmentDialog_->setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));
    auto* equipLayout = new QVBoxLayout(equipmentDialog_);
    equipLayout->setContentsMargins(0, 0, 0, 0);
    equipLayout->addWidget(equipmentWidget_);
    addCloseBar(equipmentDialog_, equipLayout);

    // 智能识别页（二维码 + OCR + 人员点名 + 设备盘点 四卡片）
    // 可用性在构造时传入：服务先初始化完，卡片一出现就是终态（置灰卡片不响应点击）
    smartRecognitionWidget_ = new SmartRecognitionWidget(workspace_, rollCallOk, equipmentOk);
    connect(smartRecognitionWidget_, &SmartRecognitionWidget::rollCallRequested,
            this, &frmMain::showRollCallDialog);
    connect(smartRecognitionWidget_, &SmartRecognitionWidget::equipmentRequested,
            this, &frmMain::showEquipmentDialog);
    smartPage_ = new QWidget();
    smartPage_->setLayout(new QVBoxLayout());
    smartPage_->layout()->addWidget(smartRecognitionWidget_);
    ui->stackedWidget->addWidget(smartPage_);
}

// 智能识别页卡片入口：每次打开前刷新列表，保证弹窗与数据库一致
void frmMain::showRollCallDialog()
{
    if (!rollCallDialog_) return;
    if (rollCallWidget_) rollCallWidget_->refreshTaskList();
    rollCallDialog_->show();
    rollCallDialog_->raise();
    rollCallDialog_->activateWindow();
}

void frmMain::showEquipmentDialog()
{
    if (!equipmentDialog_) return;
    if (equipmentWidget_) equipmentWidget_->refreshTaskList();
    equipmentDialog_->show();
    equipmentDialog_->raise();
    equipmentDialog_->activateWindow();
}

// 点名服务资源约定（根目录 = 环境变量 IDGE_WORKSPACE，缺省回退当前工作目录）：
//   model/face/ 下两件套权重：detection.rknn(SCRFD 检测)、recognition.rknn(特征比对)，
//   由进程内 InProcessFaceRecognizer 常驻加载（不再依赖 face_recognition 可执行文件）；
//   数据统一放 data/roll_call_data/（SQLite: roll_call.db）
bool frmMain::initRollCallService()
{
    rollCallService_ = std::make_shared<RollCallService>();
    const std::string ws = workspace_;
    const bool success = rollCallService_->initialize(
        ws + "/model/face/detection/model.rknn",
        ws + "/model/face/recognition/model.rknn",
        RuntimePaths::rollCallDb(QString::fromStdString(ws)).toStdString(),
        RuntimePaths::rollCallDir(QString::fromStdString(ws)).toStdString());
    if (success) qDebug() << "RollCallService initialized successfully";
    return success;
}

// 盘点模型清单解析（启动 init 与模型库变更 reload 共用的单一口径）：
//   - coco 槽：yolo11n-coco（80 类通用目标，当前作为盘点主力权重）；
//   - fire 槽：库内 id 含 "fire" 的模型（用户日后用库管理导入明火 .rknn 即自动生效），
//     未入库则跳过，仅用 COCO 单模型盘点。
// 核分配在服务层：首个模型绑 NPU 核0、其余核1（主检测流水线预算留核2）
std::vector<EquipmentModelConfig> frmMain::equipmentModelConfigsFromLibrary()
{
    ModelRegistry &reg = ModelRegistry::instance();
    std::vector<EquipmentModelConfig> models;
    const QString coco_id = QStringLiteral("yolo11n-coco");
    if (reg.contains(coco_id)) {
        models.push_back({"", reg.modelPath(coco_id).toStdString(),
                          reg.labelPath(coco_id).toStdString()});
    }
    QString fire_id;
    for (const QString &id : reg.ids()) {
        if (id != coco_id && id.contains("fire", Qt::CaseInsensitive)) {
            fire_id = id;
            break;
        }
    }
    if (!fire_id.isEmpty()) {
        models.push_back({"", reg.modelPath(fire_id).toStdString(),
                          reg.labelPath(fire_id).toStdString()});
    } else {
        qWarning() << "明火模型未入库，暂用 COCO 单模型盘点";
    }
    return models;
}

bool frmMain::initEquipmentService()
{
    equipmentService_ = std::make_shared<EquipmentInventoryService>(rollCallService_);
    const std::vector<EquipmentModelConfig> models = equipmentModelConfigsFromLibrary();
    if (models.empty()) {
        qWarning() << "设备盘点初始化失败：模型库中无 yolo11n-coco 也无明火模型";
        return false;
    }
    return equipmentService_->initialize(models);
}

// 盘点模型热更新：模型库导入/zip导入/删除/手动刷新后调用，免重启生效。
// 不换服务实例（initialize 已原子化：失败保持旧模型），设备页与对话框持有的
// shared_ptr 无需重新注入。并发安全：盘点选图/识别对话框均为模态 exec()，
// 批处理运行期间无法操作模型管理页；预览 PpeTask 持独立模型实例不受影响。
void frmMain::reloadEquipmentService()
{
    if (!equipmentService_) return;
    const std::vector<EquipmentModelConfig> models = equipmentModelConfigsFromLibrary();
    // 清单未变且模型已就绪：导入的是与盘点无关的模型（helmet 等），免无谓的 NPU 重载
    if (equipmentService_->isReady() && equipmentService_->modelConfigs() == models) return;
    if (models.empty() || !equipmentService_->initialize(models)) {
        log("model", "盘点模型重载失败，保持原模型继续工作");
        QMessageBox::warning(this, "盘点模型重载失败",
                             "新模型加载失败或库中无可用模型，盘点保持使用原模型");
        return;
    }
    log("model", QString("盘点模型热更新成功：%1 个模型").arg(models.size()));
}

// ==========================================
// 导航按钮点击处理
// ==========================================

/**
 * @brief 顶部导航按钮点击处理
 * 页面切换用 setCurrentWidget 按控件指针（避免索引漂移）：
 *   - "视频监控" -> pageMonitor（视频监控页）
 *   - "数据看板" -> dashboardWidget_（数据看板）
 *   - "报警数据" -> alarmListWidget_（报警数据）
 *   - "智能识别" -> smartPage_（二维码 + OCR + 人员点名 + 设备盘点）
 *   - "调试设置" -> pageSettings（调试帮助页）
 *   - btnExit（"退出登录"）-> onLogoutRequested()，**按对象身份**分派，见下
 * 人员点名/设备盘点不是导航项：它们在智能识别页里以卡片入口 + 弹窗呈现。
 */
void frmMain::buttonClick()
{
    QAbstractButton *b = (QAbstractButton *)sender();

    // 更新按钮选中状态。「退出登录」不参与：它不对应任何页面，没有选中态可给，
    // 点完就回登录页，留着高亮等于骗人说"当前在这一页"。
    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        btn->setChecked(btn == b && btn != ui->btnExit);
    }

    // 「退出登录」按**对象身份**而不是文字分派：文字这轮从"退出"改成了"退出登录"，
    // 只改文字不改这条链，这颗钮就会静默失效（点了没反应，比报错难查得多）。
    if (b == ui->btnExit) {
        onLogoutRequested();
        return;
    }

    const QString name = b->text();

    // 切换页面
    if (name == "视频监控") {
        ui->stackedWidget->setCurrentWidget(ui->pageMonitor);
        if (videoAlarmWidget_) videoAlarmWidget_->refreshAlarms();
    } else if (name == "数据看板") {
        ui->stackedWidget->setCurrentWidget(dashboardWidget_);
    } else if (name == "报警数据") {
        ui->stackedWidget->setCurrentWidget(alarmListWidget_);
    } else if (name == "智能识别") {
        ui->stackedWidget->setCurrentWidget(smartPage_);
    } else if (name == "调试设置") {
        ui->stackedWidget->setCurrentWidget(ui->pageSettings);
    }
}

// ==========================================
// 左侧导航初始化
// ==========================================

/**
 * @brief 初始化视频监控页左侧导航
 * 图标：相机(0xf030)、图片(0xf03e)、USB(0xf247)
 */
void frmMain::initLeftMain()
{
    iconsMain << 0xf030 << 0xf03e << 0xf247;

    for (int i = 0; i < btnsMain.count(); ++i) {
        QToolButton *btn = (QToolButton *)btnsMain.at(i);
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        connect(btn, SIGNAL(clicked(bool)), this, SLOT(leftMainClick()));
    }

    // 配置导航栏样式颜色
    IconHelper::StyleColor styleColor;
    styleColor.position = "left";
    styleColor.iconSize = 18;
    styleColor.iconWidth = 35;
    styleColor.iconHeight = 25;
    styleColor.borderWidth = 4;
    styleColor.borderColor = borderColor;
    styleColor.setColor(normalBgColor, normalTextColor, darkBgColor, darkTextColor);
    IconHelper::setStyle(ui->widgetLeftMain, btnsMain, iconsMain, styleColor);
}

// ==========================================
// 左侧导航点击处理
// ==========================================

void frmMain::leftMainClick()
{
    QAbstractButton *b = (QAbstractButton *)sender();
    for (int i = 0; i < btnsMain.count(); ++i) {
        QAbstractButton *btn = btnsMain.at(i);
        btn->setChecked(btn == b);
    }
}

// ==========================================
// 窗口控制按钮
// ==========================================

/**
 * @brief 系统最小化
 */
void frmMain::on_btnMenu_Min_clicked()
{
    // 全屏态下直接 showMinimized()：mutter 记着这个窗的 fullscreen flag，
    // 从 dock 点回来时又满屏，而用户点最小化想看的是桌面。先退回普通态再最小化，
    // 恢复时就是 restoreGeometry_ 那一档（连 canMove 一起由同一段代码摆平）。
    if (isFullScreen()) {
        on_btnMenu_Max_clicked();
    }
    showMinimized();
}

// ============================================================================
// on_btnMenu_Max_clicked —— 最大化/还原 = 真全屏的切换
// ============================================================================
// 为什么只有 _NET_WM_STATE_FULLSCREEN 这条路（旧实现是 setGeometry 铺满，是错的）：
//   · QtHelper::getScreenRect() 默认 available=true，取的是 QScreen::availableGeometry()，
//     而这个值**天生排除了 dock 的 strut**。所以 setGeometry(getScreenRect())
//     永远盖不掉那条 dock —— 旧"全屏"其实是"铺到 dock 之外"，实测 1864x1200+56+0，
//     左侧 56px 在十寸屏上等于整条可点区域左移并被 dock 挡住一部分。
//   · 手动 setGeometry(0,0,1920,1200) 也不行：GNOME 的 dash 是 **shell actor**，
//     合成层级在普通 X 窗之上，只有 mutter 自己判到 meta_window_is_fullscreen()
//     才会主动收起 top-bar/dash。
//   · Qt::X11BypassWindowManagerHint 同样无效：绕过 WM 不改变 actor 层级，
//     还会让窗口失去正常的摆放与焦点处理。
//   ⇒ 想让 dock 消失只能请求真全屏状态，别再试改 geometry。
//
// restoreGeometry_ 用成员而不是 static 局部函数变量：static 是"每个进程一份"，
// 本类实例确实只有一个，但把"这份还原态属于这个窗口"写在类型上才对，
// 也免得将来加第二个 frmMain 时两窗共用一份还原位。
//
// canMove 必须跟着改：appinit.cpp 的全局事件过滤器按这个 property 决定能不能
// 在标题栏拖动窗口。全屏下拖动既无意义，又会让 mutter 按移动请求重排窗口、
// 把全屏态搅成"位置怪掉的满屏窗"（dock 随之回来）。
// ============================================================================
void frmMain::on_btnMenu_Max_clicked()
{
    if (isFullScreen()) {
        showNormal();
        if (restoreGeometry_.isValid()) {
            setGeometry(restoreGeometry_);
        }
    } else {
        restoreGeometry_ = geometry();
        showFullScreen();
    }
    setProperty("canMove", !isFullScreen());
}

void frmMain::on_btnMenu_Close_clicked()
{
    systemExit();
}

/**
 * @brief 系统退出确认
 * 弹出确认对话框，用户确认后关闭窗口
 */
void frmMain::systemExit()
{
    // 顶层无父窗口 + 置顶，避免模态框被全屏无边框主窗遮挡导致"卡死"
    QMessageBox box(QMessageBox::Question, tr("退出系统"), tr("确认要退出系统吗？"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QtHelper::localizeMessageBoxButtons(&box);
    box.adjustSize();

    // 居中显示——**必须按"主窗所在的那块屏"居中，不是 primaryScreen**。
    // 本机是 DSI-1(触屏, +0+0) + HDMI-1(外接调试屏, +1920+0) 双屏，primaryScreen 恰好是 HDMI 那块，
    // 于是全屏下点关闭，确认框整块弹到**没有触摸屏的那台显示器**上（实测：框在 +2716+451），
    // 岗位上的人按不到 ×也按不到 取消，等于把唯一的退出通道堵死。
    if (QScreen *screen = QGuiApplication::screenAt(geometry().center())) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();

    if (box.exec() == QMessageBox::Ok) {
        close();
    }
}

// ==========================================
// 扫码登录门
// ==========================================
//
// 门是 stackedWidget 的第 0 页，frmMain 只构造一次。为什么不做成
// "main() 里先弹登录框再建主窗"：ConfigManager::load() 在 initForm() 里，
// 登录框在那之前拿不到摄像头地址；而"退出登录要能回到登录页"在那个方案下
// 就得反复构造 frmMain ⇒ metaType 重复注册、报警信号重复连接、四路重复开。
//
// 门挡的是"检测与报警产生"（四路解码器 + 报警限流），**不是整个进程的网络出口**：
// GB28181 与 WebSocket 连接在门外照常按配置跑，详见 src/ui/form/README.md。

void frmMain::startAllStreams()
{
    if (streamsRunning_) {
        log("system", "四路已在运行，忽略重复的 startAllStreams()");
        return;
    }

    // 必须在 config 加载之后（initForm 第 9 步）；登录成功时这个条件早已满足。
    // enabled=false 的通道留空，四宫格显示"未开流"。运行中改这个开关（以及模型列表）
    // 不需要重启：设置页会经 applyChannelConfigNow() 重开该路。
    ConfigManager &cfg = ConfigManager::instance();
    int opened = 0;
    for (int ch = 0; ch < cfg.channelCount(); ++ch) {
        if (!cfg.channelEnabled(ch))
            continue;
        videoWindow->openVideo(ch, cfg.channelSource(ch));
        ++opened;
    }
    streamsRunning_ = true;
    log("system", QString("登录后可用，已开流 %1 路（共 %2 路）")
                      .arg(opened).arg(cfg.channelCount()));
}

void frmMain::stopAllStreams()
{
    // 停止时序（两阶段并行停止，避免串行等待导致关闭时间叠加）：
    // 1. signalStop: 通知所有解码器退出（不阻塞）
    // 2. waitForStop: 等待所有解码线程实际退出
    // 这样 4 路解码器并行退出，总等待时间 ≈ max(单路) 而非 sum(4路)
    if (!streamsRunning_) {
        return;   // 没开过（或已停过）：关窗时 closeEvent 与本函数都会走到这里，重复停是无害但会打无谓日志
    }

    // 停止数据看板刷新定时器（下次切到看板页 showEvent 会自行重启，无需显式 start）
    if (dashboardWidget_) {
        dashboardWidget_->stop();
    }

    if (videoWindow) {
        for (int ch = 0; ch < 4; ch++) {
            PlayerWidget *pw = videoWindow->playerWidget(ch);
            if (pw) {
                pw->signalStop();
            }
        }
        for (int ch = 0; ch < 4; ch++) {
            PlayerWidget *pw = videoWindow->playerWidget(ch);
            if (pw) {
                pw->waitForStop();
            }
        }
    }
    streamsRunning_ = false;
}

void frmMain::showLoginGate()
{
    if (!login_) {
        return;
    }
    login_->resetStatus();

    // 光切页面挡不住：.ui 里 widgetTitle{ widgetTop, widgetMenu } 与 stackedWidget
    // **是平级的**，登录页盖不住导航行。不收起 widgetTop 的话，登录期间照样能点
    // 「智能识别」，而那页一进去就开 smartRecognition.camera 的设备节点 ——
    // 与登录页的扫码器抢同一个 /dev/videoNN（ConfigManager.h 明令禁止第二次打开）。
    // 角标 alarmBadge_ 是 btnEquipCheck 的子件，跟着一起藏，不用单独处理。
    // 标题栏三钮区 widgetMenu 留着：× 就是退出程序，与登录页那颗按钮同义、不冲突。
    if (ui->widgetTop) {
        ui->widgetTop->hide();
    }
    // 登录人角标也一起收：它不在 widgetTop 里（是 widgetTitle 的直接子件），
    // 不单独处理的话未登录时右上角会挂着一颗"登录人 —"，看着像还登着。
    updateSessionBadge();

    ui->stackedWidget->setCurrentWidget(login_);
}

void frmMain::updateSessionBadge()
{
    if (!sessionBadge_) {
        return;
    }
    const session::ConstructionInfo info = session::ConstructionSession::instance().snapshot();
    if (!session::ConstructionSession::instance().isLoggedIn()) {
        sessionBadge_->hide();
        return;
    }
    // 只放"这一班是谁 + 哪块施工"，其余四项进详情框看：标题栏是 1920 宽里跟导航行
    // 抢横向空间的一方，全字段铺开就会截字（截了字就等于没有这个提示）。
    sessionBadge_->setText(QString::fromUtf8("登录人 %1 · %2")
                               .arg(session::ConstructionInfo::display(info.workerName),
                                    session::ConstructionInfo::display(info.constructionId)));
    sessionBadge_->show();
    sessionBadge_->raise();
}

void frmMain::openSessionDetail()
{
    ConstructionDetailDialog dialog(this);
    dialog.exec();
}

void frmMain::onLoginSucceeded()
{
    if (ui->widgetTop) {
        ui->widgetTop->show();
    }
    updateSessionBadge();

    ui->stackedWidget->setCurrentWidget(ui->pageMonitor);
    // 走按钮的既有分派（含 setChecked 归位与右侧报警面板刷新），和启动时同一条路径
    ui->btnMain->click();

    startAllStreams();
    log("system", QString("扫码登录成功：%1")
                      .arg(session::ConstructionSession::instance().summary()));
}

void frmMain::onLogoutRequested()
{
    QMessageBox box(QMessageBox::Question, tr("退出登录"),
                    tr("退出登录后将停止视频检测，直到重新扫码登录。\n确认退出登录吗？"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QtHelper::localizeMessageBoxButtons(&box);
    box.adjustSize();
    // 与 systemExit() 同一条居中口径：按主窗所在屏，不是 primaryScreen（本机是双屏，
    // primary 恰好是没触摸的那块，弹过去岗位上按不到）
    if (QScreen *screen = QGuiApplication::screenAt(geometry().center())) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();
    if (box.exec() != QMessageBox::Ok) {
        return;
    }

    stopAllStreams();
    // 限流窗口是"通道:类别"键的纯内存态，与施工信息无关：不清的话重登后
    // 2 秒（检测）/围栏窗口期内的第一条报警会被静默吞掉，界面一条都不出。
    AlarmManager::instance().resetThrottles();
    session::ConstructionSession::instance().logout();

    // 悬浮提示收掉：它是 this 的子件、盖在整窗之上，不藏会在登录页右上角继续挂着上一班的报警
    if (toastTimer_) {
        toastTimer_->stop();
    }
    if (alarmToast_) {
        alarmToast_->hide();
    }

    showLoginGate();
    log("system", "已退出登录：四路已停、限流窗口已清、施工信息已作废");
}

void frmMain::closeEvent(QCloseEvent *event)
{
    // 停流序列抽到 stopAllStreams()（与"退出登录"共用同一段，见那边的说明）
    stopAllStreams();
    event->accept();
    qApp->quit();
}

void frmMain::on_pageRoll_customContextMenuRequested(const QPoint &pos)
{
}

// ==========================================
// 调试帮助页初始化
// ==========================================

// 当前配置解析出的推理任务总数 = 各通道实际用到的模型数之和。
// 一个任务一个 PpeTask 线程，所以这个数字就是"推理线程数"的真实值；
// config 的 detect.threads 从来没人读，别再拿它当答案。
static int configuredTaskCount()
{
    ConfigManager &cfg = ConfigManager::instance();
    int n = 0;
    for (int ch = 0; ch < cfg.channelCount(); ++ch)
        n += (int)ModelRegistry::instance().resolveChannelSlots(ch).size();
    return n;
}

/**
 * @brief 初始化设置页
 *
 * 从 config.json 读取配置并填充 UI（schema V2：一切按通道 + 按模型）：
 *   - 系统信息：平台、NPU核心数、视频接入方式、最大通道数
 *   - 视频通道：每路的 源地址 / 备注 / 启用 / 级联模型列表（可勾选可排序）
 *   - 模型参数：一行一个模型库 id，报警类别与备注写在这里
 *   - 模型库管理：库内模型的 类别数 / 输入尺寸 / 被引用通道
 *   - 电子围栏：围栏类别（单值，默认 person）+ 报警最小间隔秒数
 *   - 检测配置：置信度阈值、NMS 阈值（当前未接入推理，见 README 已知问题）
 *
 * 同时输出初始日志到运行日志面板
 */
void frmMain::initDebugPage()
{
    ConfigManager &cfg = ConfigManager::instance();

    // ========== 系统信息（固定值） ==========
    ui->labPlatformVal->setText("RK3588");
    ui->labNpuCoresVal->setText(QString::number(NPU_CORE_NUM));
    ui->labVideoInVal->setText("RTSP / V4L2");
    ui->labMaxChannelsVal->setText("4");

    // ========== 为 pageSettings 添加滚动支持 ==========
    {
        QVBoxLayout *pageSettingsLayout = qobject_cast<QVBoxLayout *>(ui->pageSettings->layout());
        if (pageSettingsLayout) {
            QScrollArea *scrollArea = new QScrollArea(ui->pageSettings);
            scrollArea->setWidgetResizable(true);
            scrollArea->setFrameShape(QFrame::NoFrame);
            scrollArea->setStyleSheet("QScrollArea { background: transparent; border: none; }");

            QWidget *scrollContent = new QWidget();
            scrollContent->setStyleSheet("background: transparent;");
            QVBoxLayout *contentLayout = new QVBoxLayout(scrollContent);
            contentLayout->setContentsMargins(0, 0, 0, 0);
            contentLayout->setSpacing(6);

            // 将 pageSettings 中已有的控件移到 scrollContent
            QLayoutItem *item;
            while ((item = pageSettingsLayout->takeAt(0)) != nullptr) {
                if (item->widget()) {
                    contentLayout->addWidget(item->widget());
                } else if (item->layout()) {
                    contentLayout->addLayout(item->layout());
                }
                delete item;
            }
            contentLayout->addStretch();

            scrollArea->setWidget(scrollContent);
            pageSettingsLayout->addWidget(scrollArea);
        }
    }

    // ========== 视频通道（config 的 video.channels，下标即通道号） ==========
    // 模型路径/标签文件/输入尺寸/模型类型四行在本轮 schema 里失去意义：
    // config 只写模型库目录名，路径与标签由 model/library 解析，所以整行隐藏。
    for (int i = 0; i < 4; ++i) {
        QLineEdit *chEdit = channelSourceEdit(i);
        chEdit->setText(cfg.channelSource(i));
    }
    for (QWidget *w : {static_cast<QWidget *>(ui->labModelPath),
                       static_cast<QWidget *>(ui->lineEditModelPath),
                       static_cast<QWidget *>(ui->btnBrowseModel),
                       static_cast<QWidget *>(ui->labLabelPath),
                       static_cast<QWidget *>(ui->lineEditLabelPath),
                       static_cast<QWidget *>(ui->btnBrowseLabel),
                       static_cast<QWidget *>(ui->labInputSize),
                       static_cast<QWidget *>(ui->labInputSizeVal),
                       static_cast<QWidget *>(ui->labModelType),
                       static_cast<QWidget *>(ui->labModelTypeVal),
                       static_cast<QWidget *>(ui->labClassNum),
                       static_cast<QWidget *>(ui->labClassNumVal)}) {
        w->setVisible(false);
    }
    if (QGroupBox *grp = qobject_cast<QGroupBox *>(ui->gridLayout_model->parentWidget()))
        grp->setTitle("视频通道（源 / 备注 / 启用 / 级联模型）");

    // ========== 检测配置（从 config.json 读取） ==========
    // 触屏够得着的最低值必须就是安全下限：不钉这一颗，长按 ▼ 能一路按到 0.00，
    // 而阈值归 0 会让量化模型的后处理预筛整体失效（见 ConfigManager::setConfThreshold）。
    ui->spinBoxConfThresh->setMinimum(0.05);
    // 初值必须屏蔽信号：这两颗 spinbox 的 valueChanged 会 setConfThreshold + save()，
    // 而 save() 是持锁把整份 JSON 截断重写（非原子）。.ui 里的默认值是 0.25、板上的
    // 实际值是 0.30，于是"开机进设置页"本身就会白写两次盘——崩溃时机不巧就损坏配置。
    ui->spinBoxConfThresh->blockSignals(true);
    ui->spinBoxNmsThresh->blockSignals(true);
    ui->spinBoxConfThresh->setValue(cfg.confThreshold());
    ui->spinBoxNmsThresh->setValue(cfg.nmsThreshold());
    ui->spinBoxConfThresh->blockSignals(false);
    ui->spinBoxNmsThresh->blockSignals(false);
    // 类别数量 / 输入尺寸 / 模型类型不再显示：V2 里这些都是每个模型库条目的属性
    // （见"模型库管理"表的 类别数/输入 两列），全局值会误导。
    // 推理线程数同理不是配置项：一个槽位一个 PpeTask 线程，
    // 实际路数 = 各通道解析出的模型数之和，这里显示真实值而不是 config 里的占位 3。
    ui->labThreadsVal->setText(QString("%1 路（Σ各通道模型）").arg(configuredTaskCount()));

    // ========== 每路：备注 + 启用开关 + 级联模型 + 清空 ==========
    {
        QGridLayout *gl = ui->gridLayout_model;   // 现有通道所在的分组框网格
        for (int i = 0; i < 4; ++i) {
            QLineEdit *chEdit = channelSourceEdit(i);
            // 地址输入框改动即落盘（浏览按钮也会写，这里补上手输的情况）。
            // 失焦也会触发 editingFinished，值没变就不落盘、不刷日志。
            connect(chEdit, &QLineEdit::editingFinished, this, [this, i, chEdit]() {
                ConfigManager &c = ConfigManager::instance();
                const QString v = chEdit->text().trimmed();
                if (c.channelSource(i) == v)
                    return;
                c.setChannelSource(i, v);
                c.save();
                log("system", QStringLiteral("通道%1 视频源已保存: %2（点『浏览』可立即重开本路，否则重启后生效）")
                                .arg(i + 1).arg(v.isEmpty() ? QStringLiteral("(空)") : v));
            });

            // 备注输入框（编辑完自动保存）
            chNoteEdit_[i] = new QLineEdit(cfg.channelNote(i));
            chNoteEdit_[i]->setPlaceholderText("备注");
            chNoteEdit_[i]->setMaximumWidth(260);   // 180→260：内边距抬到 T2 后留给文字的宽度
            chNoteEdit_[i]->setStyleSheet(theme::field(theme::BORDER));
            connect(chNoteEdit_[i], &QLineEdit::editingFinished, this, [this, i]() {
                ConfigManager::instance().setChannelNote(i, chNoteEdit_[i]->text().trimmed());
                ConfigManager::instance().save();
            });
            gl->addWidget(chNoteEdit_[i], i, 3);

            // 启用开关：即时生效——停用即停本路，启用即按当前源地址重开本路
            // 皮肤层对 QCheckBox 只有 ::indicator 尺寸、没有整件高度档，所以这里必须自己抬：
            // 实测 87x42 = 4.7mm 低于 6mm 下限，而它决定的是"这一路跑不跑检测"。
            QCheckBox *enableBox = new QCheckBox("启用");
            enableBox->setChecked(cfg.channelEnabled(i));
            enableBox->setMinimumHeight(theme::TOUCH_FIELD_H);
            enableBox->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY));
            connect(enableBox, &QCheckBox::toggled, this, [this, i](bool on) {
                ConfigManager &c = ConfigManager::instance();
                c.setChannelEnabled(i, on);
                c.save();
                log("system", QString("通道%1 已%2").arg(i + 1).arg(on ? "启用" : "停用"));
                applyChannelConfigNow(i);
            });
            gl->addWidget(enableBox, i, 4);

            // 级联模型：勾选 + 排序，行序即槽位顺序（= result_id）
            // 原来 setMaximumWidth(64) 撞上新 padding(8x14) 会把"模型…"吃掉，
            // 改成只给下限、让宽度跟着文字走 —— 触屏上截字是硬伤。
            QPushButton *modelBtn = new QPushButton("模型…");
            modelBtn->setMinimumWidth(88);
            modelBtn->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
            connect(modelBtn, &QPushButton::clicked, this,
                    [this, i]() { openChannelModelsDialog(i); });
            gl->addWidget(modelBtn, i, 5);

            // 清空按钮：清掉该路的视频路径 + 备注
            QPushButton *clearBtn = new QPushButton("清空");
            clearBtn->setMinimumWidth(88);
            clearBtn->setStyleSheet(theme::mainButton(theme::DANGER));
            connect(clearBtn, &QPushButton::clicked, this, [this, i, chEdit]() {
                chEdit->clear();
                chNoteEdit_[i]->clear();
                ConfigManager &c = ConfigManager::instance();
                c.setChannelSource(i, "");
                c.setChannelNote(i, "");
                c.save();
                log("system", QString("通道%1 视频源与备注已清空").arg(i + 1));
            });
            gl->addWidget(clearBtn, i, 6);
        }
    }

    // ========== 模型库管理区 + 模型参数表 ==========
    // 两者都插在"视频通道"分组框之后，所以先插库、后插参数表，
    // 页面上的先后顺序才是 通道 → 模型参数 → 模型库。
    initModelLibraryUi();
    initModelParamsUi();

    // ========== 围栏配置区（独立于级联） ==========
    initGeofenceUi();

    // ========== 报警类别已按模型配置（见"模型参数"表），隐藏旧的全局报警控件 ==========
    if (ui->editAlarmClasses) ui->editAlarmClasses->setVisible(false);
    if (ui->btnSaveAlarmClasses) ui->btnSaveAlarmClasses->setVisible(false);
    if (ui->labAlarmHint) ui->labAlarmHint->setVisible(false);

    // ========== WebSocket 配置 ==========
    {
        QGroupBox *grpWs = new QGroupBox("WebSocket 配置");
        grpWs->setStyleSheet(QString("QGroupBox{font-size:%1px; font-weight:bold;}"
                                     "QLabel{font-size:%2px;}"
                                     "QLineEdit{font-size:%2px;}"
                                     "QPushButton{font-size:%2px;}")
                                 .arg(theme::FS_CARD)
                                 .arg(theme::FS_BODY));
        QGridLayout *glWs = new QGridLayout(grpWs);
        glWs->setHorizontalSpacing(10);
        glWs->setVerticalSpacing(6);

        // 服务器地址
        QLabel *labWsUrl = new QLabel("服务器地址:");
        labWsUrl->setMinimumWidth(90);
        glWs->addWidget(labWsUrl, 0, 0);

        wsUrlEdit_ = new QLineEdit();
        wsUrlEdit_->setPlaceholderText("ws://192.168.x.x:port");
        wsUrlEdit_->setText(cfg.wsServerUrl());
        wsUrlEdit_->setStyleSheet(theme::field(theme::PANEL));
        glWs->addWidget(wsUrlEdit_, 0, 1, 1, 2);

        QPushButton *btnWsSave = new QPushButton("保存");
        btnWsSave->setMinimumWidth(88);   // 上限 60 会在新 padding 下截字，改给下限
        btnWsSave->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        glWs->addWidget(btnWsSave, 0, 3);

        // 连接状态
        QLabel *labWsStatus = new QLabel("连接状态:");
        glWs->addWidget(labWsStatus, 1, 0);

        wsStatusLabel_ = new QLabel("○ 未连接");
        wsStatusLabel_->setStyleSheet("color: #dc3232;");
        glWs->addWidget(wsStatusLabel_, 1, 1);

        wsConnectBtn_ = new QPushButton("连接");
        wsConnectBtn_->setMinimumWidth(88);
        wsConnectBtn_->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        glWs->addWidget(wsConnectBtn_, 1, 3);

        // 保存按钮
        connect(btnWsSave, &QPushButton::clicked, this, [this]() {
            QString url = wsUrlEdit_->text().trimmed();
            ConfigManager &c = ConfigManager::instance();
            c.setWsServerUrl(url);
            c.save();
            log("system", QString("WebSocket 服务器地址已保存: %1").arg(url.isEmpty() ? "(空)" : url));
            // 重新连接
            WsClient::instance().stop();
            if (!url.isEmpty()) {
                WsClient::instance().start();
            }
        });

        // 连接/断开按钮
        connect(wsConnectBtn_, &QPushButton::clicked, this, [this]() {
            if (WsClient::instance().isConnected()) {
                WsClient::instance().stop();
                wsStatusLabel_->setText("○ 未连接");
                wsStatusLabel_->setStyleSheet("color: #dc3232;");
                wsConnectBtn_->setText("连接");
                log("system", "WebSocket: 手动断开连接");
            } else {
                QString url = wsUrlEdit_->text().trimmed();
                if (url.isEmpty()) {
                    log("warning", "WebSocket: 请先配置服务器地址");
                    return;
                }
                ConfigManager &c = ConfigManager::instance();
                c.setWsServerUrl(url);
                c.save();
                WsClient::instance().start();
            }
        });

        // WebSocket 信号 -> 更新状态
        connect(&WsClient::instance(), &WsClient::connected, this, [this]() {
            wsStatusLabel_->setText("● 已连接");
            wsStatusLabel_->setStyleSheet("color: #32b432;");
            wsConnectBtn_->setText("断开连接");
        });
        connect(&WsClient::instance(), &WsClient::disconnected, this, [this]() {
            wsStatusLabel_->setText("○ 未连接");
            wsStatusLabel_->setStyleSheet("color: #dc3232;");
            wsConnectBtn_->setText("连接");
        });

        // 流列表展示
        connect(&WsClient::instance(), &WsClient::streamsReceived, this,
            [this](const QJsonArray &streams) {
                log("system", QString("收到 %1 路视频流:").arg(streams.size()));
                for (const auto &s : streams) {
                    QJsonObject obj = s.toObject();
                    log("info", QString("  通道%1: %2 (%3)")
                        .arg(obj["stream_id"].toString())
                        .arg(obj["name"].toString())
                        .arg(obj["url"].toString()));
                }
            });

        // 插入到 pageSettings 布局
        QVBoxLayout *pageSettingsLayout = qobject_cast<QVBoxLayout *>(
            ui->pageSettings->layout());
        if (pageSettingsLayout) {
            int count = pageSettingsLayout->count();
            pageSettingsLayout->insertWidget(count - 1, grpWs);
        }
    }

    // ========== 初始日志（带颜色） ==========
    log("system", "系统启动");
    log("system", "平台: RK3588, NPU核心数: " + QString::number(NPU_CORE_NUM));
    log("system", "视频解码器初始化完成");
    log("system", QString("推理任务: %1 路（每个模型一个线程）").arg(configuredTaskCount()));

    // 各通道级联模型（路径由 model/library 解析，这里只列库 id 便于现场核对）
    for (int ch = 0; ch < cfg.channelCount(); ++ch) {
        const QStringList ids = cfg.channelModelIds(ch);
        log("info", QString("通道%1 模型: %2")
                .arg(ch + 1)
                .arg(ids.isEmpty() ? "(未配置，回退库内默认模型)" : ids.join(" → ")));
    }

    // 围栏配置信息
    {
        auto &fm = geofence::FenceManager::instance();
        // 不再打"模式"：报警规则由每个形状自带的角色决定，一条通道可以同时有
        // 禁区（进去就报）和岗区域（在外面就报），所以这里报的是形状清点。
        int forbidden = 0, workzone = 0, channels = 0;
        for (int ch = 0; ch < cfg.channelCount(); ++ch) {
            if (!fm.hasFence(ch)) continue;
            channels++;
            const geofence::ChannelFence cf = fm.channelFence(ch);
            for (const auto &sh : cf.shapes) {
                if (sh.role == geofence::FenceRole::Workzone) workzone++;
                else forbidden++;
            }
        }
        log("fence", QString("围栏配置: 启用=%1, 围栏类别=%2, 最小间隔=%3秒, "
                             "有围栏通道=%4个, 禁区=%5个, 岗区域=%6个")
            .arg(fm.enabled() ? "是" : "否")
            .arg(cfg.geofenceAlarmClass())
            .arg(cfg.geofenceThrottleSeconds())
            .arg(channels).arg(forbidden).arg(workzone));
    }
}

// ============================================================
// channelSourceEdit: 通道号（0-based）→ "视频通道"分组框里的源地址输入框
//   .ui 里四行控件叫 lineEditCh1~4（1 基命名），程序内部一律 0 基，
//   换算只留这一处。
// ============================================================
QLineEdit *frmMain::channelSourceEdit(int channel) const
{
    switch (channel) {
    case 0: return ui->lineEditCh1;
    case 1: return ui->lineEditCh2;
    case 2: return ui->lineEditCh3;
    case 3: return ui->lineEditCh4;
    default: return nullptr;
    }
}

// ============================================================
// applyChannelConfigNow: 让某一路按当前 config 重新拉起来
// ============================================================
// 语义是"重开本路"：停解码线程 → 释放该路全部推理任务 → 按 config 重新开流。
// 下一次 start() 里 buildCascadeTasks() 见 tasks_ 为空，就会按新配置重建槽位
// （数量、顺序、类名表、报警类别、围栏前置告警全部重算一遍）。
//
// 为什么不在跑着的解码线程里原地增删槽位：stop/signalStop/waitForStop 都从
// GUI 线程遍历 tasks_，解码线程同时也在读它，改形状只允许发生在解码循环
// 已退出之后 —— 所以整条热更新路径都必须"先停干净，再动容器"。
//
// 这里的阻塞是刻意的：waitForStop 有界（3s + 10s + 1.5s×槽位），正常 <1 秒。
// 所以日志必须先打再停，否则现场只看到"点了没反应"。
//
// 前提：调用方已把改动写进 ConfigManager 并 save()，本函数只读配置、不写。
// ============================================================
void frmMain::applyChannelConfigNow(int channel)
{
    PlayerWidget *pw = videoWindow ? videoWindow->playerWidget(channel) : nullptr;
    FFmpegVideoDecoder *dec = pw ? pw->decoder() : nullptr;
    if (!dec) {
        log("system", QStringLiteral("通道%1 没有解码器，配置将在下次开流时生效").arg(channel + 1));
        return;
    }

    log("system", QStringLiteral("通道%1 正按新配置重开本路…").arg(channel + 1));
    pw->signalStop();
    pw->waitForStop();
    if (dec->isLoopAlive()) {
        // 上一轮解码卡在驱动里没退（此时 releaseTasks/start 都不安全）：交给下一次点击
        log("system", QStringLiteral("通道%1 解码线程仍在退出中，本次重开已跳过（配置已保存，稍后再改一次即可生效）")
                            .arg(channel + 1));
        return;
    }
    dec->releaseTasks();

    ConfigManager &cfg = ConfigManager::instance();
    const QString source = cfg.channelSource(channel);
    const QStringList ids = cfg.channelModelIds(channel);
    if (!cfg.channelEnabled(channel))
        log("system", QStringLiteral("通道%1 已停用，本路不开流").arg(channel + 1));
    else if (source.isEmpty())
        log("system", QStringLiteral("通道%1 没有视频源地址，本路不开流").arg(channel + 1));
    else {
        videoWindow->openVideo(channel, source);
        log("system", QStringLiteral("通道%1 已重开: %2 ｜ 模型 %3")
                            .arg(channel + 1)
                            .arg(source)
                            .arg(ids.isEmpty() ? QStringLiteral("(未配置，回退库内默认模型)")
                                               : ids.join(QStringLiteral(" → "))));
    }
    // 界面显示的"推理线程数 = Σ各通道模型"，重建后必须跟着刷新，否则是旧值
    if (ui->labThreadsVal)
        ui->labThreadsVal->setText(QString("%1 路（Σ各通道模型）").arg(configuredTaskCount()));

    // 上面两个"本路不开流"的分支只打了日志，四宫格那边没有任何回调 ——
    // 不刷一次的话，格子会一直挂着停用前的"通道N 信号中断"，
    // 用户看不到"未启用/未配置地址"这两种他刚刚亲手设的态。
    if (pw) pw->updatePlaceholder();
}

// ============================================================
// pushSlotParamsToChannel: 把当前配置解析出的槽位运行期参数推给某一路
// ============================================================
// 报警类别与置信度是同一类东西：都是"不改槽位数、下一帧就该生效"的每槽参数，
// 所以走同一条在线换通路（一次 resolveChannelSlots + 一次 updateSlotParams），
// 不是一个改动一个函数——两套快照早晚各说各话。
// 返回 false 有两种含义：该路没在跑、或形状不符已转整路重开，调用方按日志区分。
// ============================================================
bool frmMain::pushSlotParamsToChannel(int ch)
{
    PlayerWidget *pw = videoWindow ? videoWindow->playerWidget(ch) : nullptr;
    FFmpegVideoDecoder *dec = pw ? pw->decoder() : nullptr;
    if (!dec)
        return false;

    std::vector<SlotRuntimeParam> params;
    // 变量名不能叫 slots：Qt 把 slots 定义成宏（qobjectdefs.h）
    const QList<ResolvedSlot> resolved = ModelRegistry::resolveChannelSlots(ch);
    params.reserve(resolved.size());
    for (const ResolvedSlot &s : resolved) {
        SlotRuntimeParam p;
        p.alarmClass = s.alarmClass;
        p.confThreshold = s.confThreshold;
        p.nmsThreshold = s.nmsThreshold;
        params.push_back(p);
    }
    if (dec->updateSlotParams(params))
        return true;

    if (dec->isLoopAlive()) {
        log("system", QStringLiteral("通道%1 的槽位数与配置不一致，本路按新配置重开").arg(ch + 1));
        applyChannelConfigNow(ch);
        return false;
    }
    // 该路当前没在跑。注意这里**不等于**"下次开流自然读新配置"：stop() 不清 tasks_，
    // 而 buildCascadeTasks() 见 tasks_ 非空就早退，所以"停过但没释放"的路会带着
    // 旧参数重跑。本轮不改变这个行为（重开一次要漏一份 NPU 上下文，见 plan/roadmap 遗留项）。
    return false;
}

// ============================================================
// applyModelParamsNow: 某模型的报警类别 / 置信度改动即时生效
// ============================================================
// 两者都不动模型、不动槽位数，可以在解码线程继续跑的同时换掉：解码器每帧在锁内
// 拷一份运行期参数，既构造报警上下文、也填进 TaskData 的阈值，下一帧起就是新值。
// 只碰"引用该模型的通道"，其余路与本条改动无关。
// ============================================================
void frmMain::applyModelParamsNow(const QString &libId)
{
    ConfigManager &cfg = ConfigManager::instance();
    const QList<int> users = cfg.modelParamUsers(libId);
    if (users.isEmpty()) {
        log("system", QStringLiteral("模型 %1 当前没有通道引用，参数将在它被勾上时生效").arg(libId));
        return;
    }

    QStringList live;
    for (int ch : users) {
        if (pushSlotParamsToChannel(ch))
            live << QString::number(ch + 1);
    }
    if (!live.isEmpty())
        log("system", QStringLiteral("模型参数（报警类别/置信度）已即时生效（无需重开流）: 通道%1")
                      .arg(live.join(QStringLiteral("、"))));
}

// ============================================================
// applyGlobalThresholdNow: 全局阈值改动推给所有通道
// ============================================================
// 改的是回落链的底座，所以受影响的是"所有没做每模型覆盖的槽位"——范围按模型枚举
// 数不出来（缺键的模型根本没有条目），只能整通道推。
// ============================================================
void frmMain::applyGlobalThresholdNow()
{
    QStringList live;
    for (int ch = 0; ch < ConfigManager::instance().channelCount(); ++ch) {
        if (pushSlotParamsToChannel(ch))
            live << QString::number(ch + 1);
    }
    if (!live.isEmpty())
        log("system", QStringLiteral("检测阈值已即时生效（无需重开流）: 通道%1")
                      .arg(live.join(QStringLiteral("、"))));
}

// ============================================================
// settingsContentLayout: 设置页滚动区的内容布局
//   initDebugPage 开头把 pageSettings 的原控件整体包进了 QScrollArea，
//   它是 pageSettings 布局的第 0 项；要往页尾加分组框就往这里插。
// ============================================================
static QVBoxLayout *settingsContentLayout(QWidget *page)
{
    QVBoxLayout *pageLayout = qobject_cast<QVBoxLayout *>(page->layout());
    QLayoutItem *first = pageLayout ? pageLayout->itemAt(0) : nullptr;
    QScrollArea *sa = first ? qobject_cast<QScrollArea *>(first->widget()) : nullptr;
    return sa ? qobject_cast<QVBoxLayout *>(sa->widget()->layout()) : nullptr;
}

// 置信度下拉的两个特殊项（存的是 QComboBox 的 itemData）：
constexpr int kConfFollowGlobal = -1;   ///< 「(跟随全局)」= 删掉 conf_threshold 这个键
constexpr int kConfCustom = -2;         ///< 手改 JSON 得到的非档位值：只显示，不是动作

// ============================================================
// touchComboQss: 表格内下拉的**本体**样式（弹层行高不在这里，见 theme::makeComboTouch）
//   padding 上下取 6：T2 的总高是 42 + 2*6 + 2*1 = 56，写成 4 就只有 52，
//   同一张表里下拉比旁边的按钮矮 4px，看着像"没跟上档"。
//   （更正：这里原先写"QString::arg 一个编号只替换一次，重复用到的值必须另编号"——
//    那句是错的。板上用 /tmp/qsschk 实测：arg() 每次替换的是**当前编号最低的那个
//    占位符的所有出现**，`%2` 写两遍两次都被替。编号递增只是为了让 .arg 链的顺序可读。）
// ============================================================
static QString touchComboQss()
{
    return QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3; border-radius: 4px;"
        "  min-height: %4px; padding: 6px 8px; font-size: %5px; }"
        "QComboBox::drop-down { width: 28px; }")
        .arg(theme::FIELD, theme::TEXT, theme::BORDER)
        .arg(theme::TOUCH_FIELD_MIN_H)
        .arg(theme::FS_BODY);
}

// ============================================================
// initModelParamsUi: "模型参数"分组框（一行一个模型库 id）
//   报警类别与置信度都是**模型的参数**而不是槽位的参数：同一个模型不管挂在哪个通道、
//   哪个槽位，触发的都是同一个类别、用的都是同一个阈值。旧 schema 把它写在
//   cascade.models[i] 上，换个槽位就换行为，语义不成立。
//   ⚠ "每模型"≠"每通道"：键是库 id，两个通道都挂同一个模型就共用同一个阈值。
//   备注只写在这里（video.models[<id>].note），路径/标签一律不出现——
//   那些是 model/library 的事。
// ============================================================
void frmMain::initModelParamsUi()
{
    QVBoxLayout *contentLayout = settingsContentLayout(ui->pageSettings);
    QWidget *modelGroup = ui->gridLayout_model->parentWidget();
    if (!contentLayout || !modelGroup) {
        qWarning() << "initModelParamsUi: settings scroll layout not found";
        return;
    }
    int insertAt = -1;
    for (int i = 0; i < contentLayout->count(); ++i) {
        if (contentLayout->itemAt(i)->widget() == modelGroup) {
            insertAt = i + 1;
            break;
        }
    }
    if (insertAt < 0) return;

    QGroupBox *group = new QGroupBox("模型参数（报警类别 / 置信度按模型配置）");
    group->setStyleSheet(theme::field(theme::PANEL));
    QVBoxLayout *v = new QVBoxLayout(group);

    modelParamTable_ = new QTableWidget(0, 6);
    modelParamTable_->setHorizontalHeaderLabels(
        {"模型库id", "报警类别", "置信度", "备注", "被引用通道", "操作"});
    modelParamTable_->verticalHeader()->setVisible(false);
    // 行高只能在这里设：CSS 的 ::item padding 是 delegate 内边距，撑不起 section 高度
    modelParamTable_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    modelParamTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelParamTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelParamTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelParamTable_->setShowGrid(false);
    // 只有"备注"这一列吃 Stretch：新插入的置信度列在 2，后面三列顺次挪位。
    // 这几处索引必须与 refreshModelParamsTable() 里的 setCellWidget 列号同步改，
    // 漏一处就是 Stretch 落到下拉上、"移除"按钮被挤没。
    modelParamTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    modelParamTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    modelParamTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    modelParamTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    modelParamTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    modelParamTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    // 行高 56 后按"表头 48 + 4 行"重算上限，否则可见行数会被行长挤没
    modelParamTable_->setMaximumHeight(300);
    modelParamTable_->setStyleSheet(QString(
        "QTableWidget { background: %1; border: 1px solid %2; color: %3; font-size: %4px; }"
        "QTableWidget::item { padding: 4px 8px; border-bottom: 1px solid %2; }"
        "QTableWidget::item:selected { background: %5; }"
        "QHeaderView::section { background: %2; color: %3; padding: 4px 8px; border: none; }")
        .arg(theme::FIELD, theme::BORDER, theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::HOVER));
    v->addWidget(modelParamTable_);

    contentLayout->insertWidget(insertAt, group);
    refreshModelParamsTable();
}

// ============================================================
// refreshModelParamsTable: 重建模型参数表
//   行 = 库内模型 ∪ 配置里残留的键。后者标红提示"参数表指向的模型已不在库中"，
//   否则现场会出现"config 里有、界面上看不到"的幽灵条目。
// ============================================================
void frmMain::refreshModelParamsTable()
{
    if (!modelParamTable_) return;
    ConfigManager &cfg = ConfigManager::instance();
    ModelRegistry &reg = ModelRegistry::instance();
    reg.rescan();   // 库目录可能被外部改动（与"模型库管理"区同一口径）

    QStringList rows = reg.ids();
    const QStringList keys = cfg.modelParamKeys();
    for (const QString &k : keys)
        if (!rows.contains(k)) rows << k;

    modelParamTable_->setRowCount(0);
    for (const QString &id : rows) {
        const int r = modelParamTable_->rowCount();
        modelParamTable_->insertRow(r);
        const bool inLib = reg.contains(id);
        // 引用名单在建列之前取一次：列 2 的 tooltip 要说清"这个值会 fan 到哪几路"，
        // 列 4 显示的也是它，两处必须同一个来源。
        const QList<int> users = cfg.modelParamUsers(id);
        QStringList chs;
        for (int ch : users) chs << QString::number(ch + 1);

        // --- 列 0：库 id（只读） ---
        QTableWidgetItem *idItem = new QTableWidgetItem(id);
        idItem->setFlags(idItem->flags() & ~Qt::ItemIsEditable);
        if (!inLib) {
            idItem->setForeground(QColor(theme::DANGER));
            idItem->setToolTip(QStringLiteral("模型不在 model/library 中，导入或删除该条目后即可消除"));
        }
        modelParamTable_->setItem(r, 0, idItem);

        // --- 列 1：报警类别 ---
        QComboBox *clsCombo = new QComboBox;
        theme::makeComboTouch(clsCombo);   // 这一列与置信度同表，弹层行高必须一起给
        clsCombo->addItem(QStringLiteral("(不报警)"), QString());
        const QStringList classes = inLib ? reg.meta(id).classes : QStringList{};
        for (const QString &c : classes)
            clsCombo->addItem(c, c);
        const QString cur = cfg.modelAlarmClass(id);
        int idx = clsCombo->findData(cur);
        if (idx < 0 && !cur.isEmpty()) {
            // 老配置里的类别可能不在换过的新模型标签里，仍显示出来（不改写配置）
            clsCombo->addItem(cur + QStringLiteral(" (不在标签)"), cur);
            idx = clsCombo->count() - 1;
        }
        clsCombo->setCurrentIndex(qMax(0, idx));
        connect(clsCombo, QOverload<int>::of(&QComboBox::activated), this,
                [this, id, clsCombo](int) {
                    ConfigManager &c = ConfigManager::instance();
                    const QString cls = clsCombo->currentData().toString();
                    c.setModelAlarmClass(id, cls);
                    c.save();
                    log("system", QString("模型 %1 报警类别: %2")
                            .arg(id, cls.isEmpty() ? QStringLiteral("(不报警)") : cls));
                    applyModelParamsNow(id);
                });
        modelParamTable_->setCellWidget(r, 1, clsCombo);

        // --- 列 2：置信度（档位点选） ---
        // 为什么是档位下拉而不是输入框：本轮不做虚拟键盘，任何输入框在 10 寸触屏上
        // 都落不了地（隔壁那两颗全局 QDoubleSpinBox 现在就是这个状态：0.25→0.95 要按 14 下 ▲）。
        // 首项「(跟随全局)」对应**删掉这个键**而不是写一个 0.25 进去——写死之后这个模型
        // 就再也不跟全局档走了，那恰恰是这一项要避免的事。
        QComboBox *confCombo = new QComboBox;
        confCombo->setStyleSheet(touchComboQss());
        theme::makeComboTouch(confCombo);
        confCombo->addItem(QStringLiteral("(跟随全局)"), kConfFollowGlobal);
        for (int step = 1; step <= 19; ++step)              // 0.05 … 0.95
            confCombo->addItem(QString::number(step / 20.0, 'f', 2), step);
        // 档位值用 step/20.0 而不是 step*0.05：后者在 double 里 3*0.05 = 0.15000000000000002，
        // 与存盘再读回来的 0.15 不是同一个数，任何按值比对的写法都会当场判成"没配"。
        const float storedConf = cfg.modelConfThreshold(id);   // 负数 = 未配置
        int confIdx = 0;
        if (storedConf >= 0.f) {
            const int step = qRound(storedConf * 20.f);
            confIdx = confCombo->findData(step);
            if (confIdx < 0 || qAbs(step / 20.0 - static_cast<double>(storedConf)) > 1e-3) {
                // 手改 JSON 改出了一个非档位值：如实列出来，不吸附到最近的档位，
                // 否则界面写着 0.35、实际跑的是 0.333，现场只会认为阈值没生效。
                confCombo->addItem(QStringLiteral("自定义 %1")
                                   .arg(QString::number(storedConf, 'f', 3)), kConfCustom);
                confIdx = confCombo->count() - 1;
            }
        }
        confCombo->setCurrentIndex(confIdx);
        confCombo->setToolTip(QStringLiteral(
            "越高框越少、漏检越多；本模型当前生效 %1（%2）。\n按模型配置：改动会同时作用于引用它的通道 %3")
            .arg(QString::number(ModelRegistry::resolveConfThreshold(id), 'f', 3))
            .arg(storedConf >= 0.f ? QStringLiteral("每模型覆盖")
                                   : QStringLiteral("来自全局 %1")
                                         .arg(cfg.confThreshold(), 0, 'f', 2))
            .arg(chs.isEmpty() ? QStringLiteral("（当前无通道引用）") : chs.join(QStringLiteral("、"))));
        connect(confCombo, QOverload<int>::of(&QComboBox::activated), this,
                [this, id, confCombo](int) {
                    const int item = confCombo->currentData().toInt();
                    if (item == kConfCustom)
                        return;      // 「自定义」只是显示项，不是一个可执行的动作
                    ConfigManager &c = ConfigManager::instance();
                    if (item == kConfFollowGlobal)
                        c.clearModelConfThreshold(id);
                    else
                        c.setModelConfThreshold(id, item / 20.0);
                    c.save();
                    log("system", QStringLiteral("模型 %1 置信度: %2（生效 %3）")
                            .arg(id,
                                 item == kConfFollowGlobal
                                     ? QStringLiteral("(跟随全局)")
                                     : QString::number(item / 20.0, 'f', 2))
                            .arg(ModelRegistry::resolveConfThreshold(id), 0, 'f', 3));
                    // 只 apply 不 refresh：refreshModelParamsTable() 里的 reg.rescan()
                    // 会在 GUI 线程重扫整库（含逐模型哈希），阈值这颗不能带上它。
                    applyModelParamsNow(id);
                });
        modelParamTable_->setCellWidget(r, 2, confCombo);

        // --- 列 3：备注 ---
        QLineEdit *noteEdit = new QLineEdit(cfg.modelParam(id).note);
        noteEdit->setPlaceholderText(QStringLiteral("备注"));
        noteEdit->setStyleSheet(theme::field(theme::PANEL));
        connect(noteEdit, &QLineEdit::editingFinished, this, [this, id, noteEdit]() {
            ConfigManager &c = ConfigManager::instance();
            c.setModelNote(id, noteEdit->text().trimmed());
            c.save();
        });
        modelParamTable_->setCellWidget(r, 3, noteEdit);

        // --- 列 4：被引用通道（显示 1~4，内部 0-based） ---
        QTableWidgetItem *refItem =
            new QTableWidgetItem(chs.isEmpty() ? QStringLiteral("-") : chs.join(QStringLiteral(",")));
        refItem->setFlags(refItem->flags() & ~Qt::ItemIsEditable);
        refItem->setToolTip(QStringLiteral("该模型被这些通道的级联列表引用；无引用时才可删除模型"));
        modelParamTable_->setItem(r, 4, refItem);

        // --- 列 5：移除该模型的配置 ---
        QPushButton *removeBtn = new QPushButton(QStringLiteral("移除"));
        removeBtn->setMinimumWidth(88);   // 上限 56 + 新 padding 会截掉"移除"二字
        removeBtn->setStyleSheet(theme::miniButton(theme::DANGER));
        connect(removeBtn, &QPushButton::clicked, this, [this, id]() {
            if (QtHelper::askConfirm(
                    this, QStringLiteral("移除模型配置"),
                    QStringLiteral("确认从模型参数表移除 %1，并从所有通道的级联列表里剔除它？\n"
                                   "（不会删除 model/library 里的模型文件）").arg(id))
                != QMessageBox::Ok)
                return;
            ConfigManager &c = ConfigManager::instance();
            // 引用名单必须在移除前取：removeModelParam 会连各通道的 models 一起剔掉，
            // 之后 modelParamUsers(id) 恒为空，就不知道该重开哪几路了。
            const QList<int> affected = c.modelParamUsers(id);
            c.removeModelParam(id);
            c.save();
            refreshModelParamsTable();
            refreshModelLibraryTable();
            log("system", QStringLiteral("模型 %1 的配置条目已移除").arg(id));
            // 少了一个槽位 = 任务集合形状变了，在线换类别这条路走不通，只能整路重开
            for (int ch : affected)
                applyChannelConfigNow(ch);
        });
        modelParamTable_->setCellWidget(r, 5, removeBtn);
    }
}

// ============================================================
// openChannelModelsDialog: 某一通的级联模型编辑器（勾选 + 排序）
//   列表行序 = 槽位顺序 = TaskConfig.result_id = 画面上的 M1/M2/…，
//   所以这里必须能上下移动，而不是只勾不排序。
// ============================================================
void frmMain::openChannelModelsDialog(int channel)
{
    ConfigManager &cfg = ConfigManager::instance();
    ModelRegistry &reg = ModelRegistry::instance();
    reg.rescan();
    const QStringList libIds = reg.ids();
    if (libIds.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("通道级联模型"),
                             QStringLiteral("模型库为空，请先在『模型库管理』导入模型"));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("通道%1 级联模型").arg(channel + 1));
    // 触屏档：520x420 是按鼠标时代定的，10 寸屏上列表行高 ~30、勾选靶心 ~13px，
    // 等于"看得清但点不中"。1080x900 在 1920x1200 全屏下四周仍留白充足。
    dlg.resize(1080, 900);
    QVBoxLayout *v = new QVBoxLayout(&dlg);

    QListWidget *list = new QListWidget;
    list->setStyleSheet(theme::field(theme::PANEL));
    // 已配置的排在最前（保持原顺序），其余库内模型跟在后面且未勾选
    const QStringList current = cfg.channelModelIds(channel);
    QStringList ordered;
    for (const QString &id : current)
        if (libIds.contains(id)) ordered << id;
    for (const QString &id : libIds)
        if (!ordered.contains(id)) ordered << id;

    // 勾选态的区分手段：**加粗 + 提亮**，不是换底色。
    // 换底色这条路本机实测走不通：list 挂了 theme::field() 的 QSS，样式表一旦
    // 管住 item 的 background，QListWidgetItem::setBackground 的 brush 就被忽略，
    // 而 setForeground 仍然生效 —— 结果是"选中行底色没变、字却成了黑的"，
    // 深底深字直接读不了（/tmp/ui2_after/model_dlg 逐像素核对过：四行底色全是 PANEL）。
    // 所以底色交给 QSS，这里只动字：加粗给"会被跑"，暗色给"不会跑"。
    const QColor checkedFg = QColor(QString::fromLatin1(theme::TEXT));
    const QColor normalFg = QColor(QString::fromLatin1(theme::TEXT_MUTED));

    // 把"勾选 + 字形"绑成一个动作：任何改状态的路径都必须走这里，否则字的
    // 勾选态和 CheckStateRole 会各说各话（上移/下移不动状态，所以只有两处调用）。
    auto applyChecked = [checkedFg, normalFg](QListWidgetItem *row) {
        const bool on = row->checkState() == Qt::Checked;
        row->setForeground(on ? checkedFg : normalFg);
        QFont f = row->font();
        f.setBold(on);
        row->setFont(f);
    };

    for (const QString &id : ordered) {
        QListWidgetItem *item = new QListWidgetItem(list);
        const QString alarm = cfg.modelAlarmClass(id);
        item->setText(QStringLiteral("%1  ·  %2  ·  报警:%3")
                          .arg(id,
                               reg.displayName(id),
                               alarm.isEmpty() ? QStringLiteral("(无)") : alarm));
        item->setData(Qt::UserRole, id);
        // T1 档行高（54 + 2*8 + 2*1 = 72px ≈ 8.1mm）。列表行高由 sizeHint 定，
        // 不写就还是样式管不到的默认 ~30px —— 触屏上点不中的第二个原因。
        item->setSizeHint(QSize(0, 72));
        // 刻意**清掉** ItemIsUserCheckable：靶心照样画（本机实测两种 flag 下
        // 左侧像素完全一致，见 src/ui/form/README.md），但视图不再自己响应点击去切换，
        // 切换统一由下面的 itemClicked 负责。留着它 = 视图切一次、我再取反一次 = 点了没反应。
        item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        item->setCheckState(current.contains(id) ? Qt::Checked : Qt::Unchecked);
        applyChecked(item);
    }
    v->addWidget(list);

    // 摘要行：触屏上"顺序 = 槽位号"只看行位置容易看错，写成一行文字最省眼力
    QLabel *summary = new QLabel(&dlg);
    summary->setWordWrap(true);
    summary->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    auto updateSummary = [list, summary]() {
        QStringList picked;
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem *item = list->item(i);
            if (item->checkState() == Qt::Checked)
                picked << item->data(Qt::UserRole).toString();
        }
        summary->setText(QStringLiteral("已选顺序：")
                         + (picked.isEmpty()
                                ? QStringLiteral("(未勾选，确定后该路将回退库内默认模型)")
                                : picked.join(QStringLiteral(" → "))));
    };
    v->addWidget(summary);

    // 点整行任意位置切换勾选 —— 本条需求在触屏上的真正修复（放大只是看得清，
    // 点得中靠的是这个）。itemClicked 在视图自己的 edit() 之后发射，
    // 而 edit() 因为 flag 被清不会再改状态，所以这里是唯一的改状态点。
    connect(list, &QListWidget::itemClicked, this,
            [applyChecked, updateSummary](QListWidgetItem *item) {
                if (!item) return;
                item->setCheckState(item->checkState() == Qt::Checked
                                        ? Qt::Unchecked : Qt::Checked);
                applyChecked(item);
                updateSummary();
            });
    updateSummary();

    QHBoxLayout *order = new QHBoxLayout;
    QPushButton *upBtn = new QPushButton(QStringLiteral("上移"));
    QPushButton *downBtn = new QPushButton(QStringLiteral("下移"));
    // 原来是 theme::miniButton（表格内 T3 档 48px），在这个大框里显小；
    // 改成吃全局 T1 并给下限，跟列表行同高才好按。
    upBtn->setMinimumSize(140, 72);
    downBtn->setMinimumSize(140, 72);
    connect(upBtn, &QPushButton::clicked, this, [list, updateSummary]() {
        const int r = list->currentRow();
        if (r > 0) {
            list->insertItem(r - 1, list->takeItem(r));
            list->setCurrentRow(r - 1);
            updateSummary();
        }
    });
    connect(downBtn, &QPushButton::clicked, this, [list, updateSummary]() {
        const int r = list->currentRow();
        if (r >= 0 && r < list->count() - 1) {
            list->insertItem(r + 1, list->takeItem(r));
            list->setCurrentRow(r + 1);
            updateSummary();
        }
    });
    order->addWidget(upBtn);
    order->addWidget(downBtn);
    order->addStretch();
    v->addLayout(order);

    // 这个列表是对话框里按需 new 的，启动时那次全量扫描（frmMain 构造末尾）扫不到它
    QtHelper::enableTouchScroll(list);

    QDialogButtonBox *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    // 按钮文案显式中文，与 QtHelper::localizeMessageBoxButtons 那套口径一致
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(box);

    if (dlg.exec() != QDialog::Accepted)
        return;

    QStringList picked;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *item = list->item(i);
        if (item->checkState() == Qt::Checked)
            picked << item->data(Qt::UserRole).toString();
    }
    ConfigManager &c = ConfigManager::instance();
    c.setChannelModelIds(channel, picked);
    c.save();
    refreshModelParamsTable();
    refreshModelLibraryTable();
    log("system", QStringLiteral("通道%1 级联模型: %2")
                    .arg(channel + 1)
                    .arg(picked.isEmpty() ? QStringLiteral("(空，将回退库内默认模型)")
                                          : picked.join(QStringLiteral(" → "))));
    // 模型列表决定推理任务的槽位数与类名表，只能重建任务集合 → 重开本路
    applyChannelConfigNow(channel);
}

// ============================================================
// initGeofenceUi: 围栏配置区（独立于级联模型）
//   围栏不属于 ppe/coco 这些级联槽位：它复用本通道已有的检测结果，只做几何
//   判定，所以类别是**单值**（默认 person），另加一个报警最小间隔（进入沿才是主判据）。
// ============================================================
void frmMain::initGeofenceUi()
{
    ConfigManager &cfg = ConfigManager::instance();
    QVBoxLayout *contentLayout = settingsContentLayout(ui->pageSettings);
    if (!contentLayout) {
        qWarning() << "initGeofenceUi: settings scroll layout not found";
        return;
    }

    QGroupBox *group = new QGroupBox("电子围栏（独立于级联模型）");
    group->setStyleSheet(theme::field(theme::PANEL));
    QGridLayout *gl = new QGridLayout(group);
    gl->setHorizontalSpacing(10);
    gl->setVerticalSpacing(6);

    QLabel *labCls = new QLabel(QStringLiteral("围栏类别:"));
    labCls->setMinimumWidth(120);
    gl->addWidget(labCls, 0, 0);

    QComboBox *clsCombo = new QComboBox;
    clsCombo->setStyleSheet(theme::field(theme::PANEL));
    // 弹层行高 + 不吃滚轮：这一颗改的是"围栏拿哪个类别去比对检测结果"，
    // 滚一下就把围栏判据换掉并当场写盘，和模型参数表里那颗置信度下拉同一类事故。
    theme::makeComboTouch(clsCombo);
    // 候选 = 四个通道用到的模型类名并集（围栏只看这一路能不能产出该类别）
    QStringList candidates;
    for (int ch = 0; ch < cfg.channelCount(); ++ch) {
        const QStringList ids = cfg.channelModelIds(ch);
        for (const QString &id : ids) {
            const ModelMeta m = ModelRegistry::instance().meta(id);
            for (const QString &c : m.classes)
                if (!candidates.contains(c)) candidates << c;
        }
    }
    const QString curCls = cfg.geofenceAlarmClass();
    if (!candidates.contains(curCls)) candidates.prepend(curCls);
    clsCombo->addItems(candidates);
    clsCombo->setCurrentText(curCls);
    gl->addWidget(clsCombo, 0, 1);

    QLabel *labThrottle = new QLabel(QStringLiteral("围栏报警最小间隔(秒):"));
    labThrottle->setMinimumWidth(120);
    gl->addWidget(labThrottle, 1, 0);

    QSpinBox *throttleSpin = new QSpinBox;
    throttleSpin->setRange(1, 600);
    throttleSpin->setValue(cfg.geofenceThrottleSeconds());
    throttleSpin->setStyleSheet(theme::field(theme::PANEL));
    gl->addWidget(throttleSpin, 1, 1);

    connect(clsCombo, &QComboBox::currentTextChanged, this, [this](const QString &cls) {
        ConfigManager &c = ConfigManager::instance();
        c.setGeofenceAlarmClass(cls.trimmed());
        c.save();
        geofence::FenceManager::instance().loadFromConfig();
        log("fence", QStringLiteral("围栏类别: %1").arg(cls.trimmed()));
    });
    connect(throttleSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int seconds) {
        ConfigManager &c = ConfigManager::instance();
        c.setGeofenceThrottleSeconds(seconds);
        c.save();
        log("fence", QStringLiteral("围栏报警最小间隔: %1 秒").arg(seconds));
    });

    contentLayout->insertWidget(contentLayout->count() - 1, group);
}

// ============================================================
// initModelLibraryUi: 模型库管理区（P2）
//   位置：设置页滚动内容中、"模型配置"分组框之后
//   构成：工具行（刷新/导入/删除 + 提示） + 只读表格
// ============================================================
void frmMain::initModelLibraryUi()
{
    QVBoxLayout *contentLayout = settingsContentLayout(ui->pageSettings);
    QWidget *modelGroup = ui->gridLayout_model->parentWidget();
    if (!contentLayout || !modelGroup) {
        qWarning() << "initModelLibraryUi: settings scroll layout not found";
        return;
    }
    int insertAt = -1;
    for (int i = 0; i < contentLayout->count(); ++i) {
        if (contentLayout->itemAt(i)->widget() == modelGroup) {
            insertAt = i + 1;
            break;
        }
    }
    if (insertAt < 0) return;

    QGroupBox *group = new QGroupBox("模型库管理");
    group->setStyleSheet(theme::field(theme::PANEL));
    QVBoxLayout *v = new QVBoxLayout(group);

    QHBoxLayout *bar = new QHBoxLayout();
    QPushButton *btnRefresh = new QPushButton("刷新");
    QPushButton *btnImport = new QPushButton("导入模型...");
    QPushButton *btnZip = new QPushButton("导入zip包...");
    QPushButton *btnTest = new QPushButton("测试选中");
    QPushButton *btnDelete = new QPushButton("删除选中");
    btnRefresh->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnImport->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnZip->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnTest->setStyleSheet(theme::mainButton(theme::SUCCESS));
    btnDelete->setStyleSheet(theme::mainButton(theme::DANGER));
    QLabel *hint = new QLabel("库目录 model/library；改选路径后重启程序生效");
    hint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    bar->addWidget(btnRefresh);
    bar->addWidget(btnImport);
    bar->addWidget(btnZip);
    bar->addWidget(btnTest);
    bar->addWidget(btnDelete);
    bar->addStretch();
    bar->addWidget(hint);
    v->addLayout(bar);

    modelLibTable_ = new QTableWidget(0, 6);
    modelLibTable_->setHorizontalHeaderLabels(
        {"id", "名称", "任务", "类别数", "输入", "被引用"});
    modelLibTable_->verticalHeader()->setVisible(false);
    modelLibTable_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    modelLibTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelLibTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelLibTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelLibTable_->setShowGrid(false);
    modelLibTable_->horizontalHeader()->setStretchLastSection(false);
    modelLibTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    // 行高 56 后按"表头 48 + 4 行"重算上限
    modelLibTable_->setMaximumHeight(280);
    modelLibTable_->setStyleSheet(QString(
        "QTableWidget { background: %1; border: 1px solid %2; color: %3; font-size: %4px; }"
        "QTableWidget::item { padding: 4px 8px; border-bottom: 1px solid %2; }"
        "QTableWidget::item:selected { background: %5; }"
        "QHeaderView::section { background: %2; color: %3; padding: 4px 8px; border: none; }")
        .arg(theme::FIELD, theme::BORDER, theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::HOVER));
    v->addWidget(modelLibTable_);

    connect(btnRefresh, &QPushButton::clicked, this, [this]() {
        refreshModelLibraryTable();
        refreshModelParamsTable();
        reloadEquipmentService();   // 手工刷库也可能对应外部已拷入的模型目录
    });
    connect(btnImport, &QPushButton::clicked, this, &frmMain::importModelViaDialog);
    connect(btnZip, &QPushButton::clicked, this, &frmMain::importModelZip);
    connect(btnTest, &QPushButton::clicked, this, &frmMain::testSelectedModel);
    connect(btnDelete, &QPushButton::clicked, this, &frmMain::deleteSelectedModel);

    contentLayout->insertWidget(insertAt, group);
    refreshModelLibraryTable();
}

// ============================================================
// refreshModelLibraryTable: rescan 库并重建表格
// ============================================================
void frmMain::refreshModelLibraryTable()
{
    if (!modelLibTable_) return;
    ModelRegistry &reg = ModelRegistry::instance();
    reg.rescan();
    modelLibTable_->setRowCount(0);
    int row = 0;
    const QStringList ids = reg.ids();
    for (const QString &id : ids) {
        const ModelMeta m = reg.meta(id);
        modelLibTable_->insertRow(row);
        const QStringList cols = {
            m.id,
            m.displayName,
            m.task,
            QString::number(m.numClasses),
            QString("%1x%2").arg(m.inputWidth).arg(m.inputHeight),
            QString::number(reg.referenceCount(id)),
        };
        for (int c = 0; c < cols.size(); ++c)
            modelLibTable_->setItem(row, c, new QTableWidgetItem(cols[c]));
        ++row;
    }
}

// ============================================================
// importModelViaDialog: 三步导入（rknn → labels(可跳过) → id）
//   importModel 内置 sha256 去重；成功后同步刷新表格与下拉
// ============================================================
void frmMain::importModelViaDialog()
{
    QString rknn = QFileDialog::getOpenFileName(
        this, "导入模型 (1/3): 选择 .rknn 文件", "model", "RKNN 模型 (*.rknn)");
    if (rknn.isEmpty()) return;

    QString labels = QFileDialog::getOpenFileName(
        this, "导入模型 (2/3): 选择标签文件（可取消=跳过）", "model",
        "文本文件 (*.txt);;所有文件 (*)");

    bool ok = false;
    QString id = QInputDialog::getText(
        this, "导入模型 (3/3): 指定 id",
        "库内目录名（小写字母/数字/-/_）:",
        QLineEdit::Normal, QFileInfo(rknn).completeBaseName().toLower(), &ok)
                     .trimmed();
    if (!ok || id.isEmpty()) return;

    QString err;
    if (ModelRegistry::instance().importModel(rknn, labels, id, err)) {
        refreshModelLibraryTable();
        refreshModelParamsTable();
        reloadEquipmentService();
        QMessageBox::information(this, "导入成功",
            err.isEmpty() ? QString("已导入模型: %1").arg(id) : err);
        log("model", QString("模型库导入: %1").arg(err.isEmpty() ? id : err));
    } else {
        QMessageBox::warning(this, "导入失败", err);
        log("model", QString("模型库导入失败: %1").arg(err));
    }
}

// ============================================================
// importModelZip: zip 模型包一键导入（换库/分发场景）
//   布局约定：包内每个模型一个目录，含 model.rknn（labels.txt 可选）；
//   容忍 zip 自带的顶层文件夹（向下多探一层）。
//   流程：解压临时目录 → 收集模型目录 → 逐个 importModel（sha256 去重）
//        → 清理临时目录 → 刷新表格/下拉
// ============================================================
void frmMain::importModelZip()
{
    const QString zip = QFileDialog::getOpenFileName(
        this, "导入 zip 模型包", "model", "Zip 模型包 (*.zip)");
    if (zip.isEmpty()) return;

    const QString tmp = QDir::temp().filePath(
        QStringLiteral("idge_model_zip_%1").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!QDir().mkpath(tmp)) {
        QMessageBox::warning(this, "解压失败", "无法创建临时目录: " + tmp);
        return;
    }
    const int rc = QProcess::execute("unzip", {"-q", "-o", zip, "-d", tmp});
    if (rc != 0) {
        QDir(tmp).removeRecursively();
        QMessageBox::warning(this, "解压失败",
            QString("unzip 退出码 %1（zip 损坏或解压工具缺失）").arg(rc));
        return;
    }

    // 收集含 model.rknn 的目录：根 → 一级子目录 → 二级子目录
    QStringList modelDirs;
    QDir t(tmp);
    if (t.exists("model.rknn")) {
        modelDirs << t.absolutePath();
    } else {
        const QFileInfoList l1 = t.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d1 : l1) {
            QDir q1(d1.absoluteFilePath());
            if (q1.exists("model.rknn")) {
                modelDirs << q1.absolutePath();
                continue;
            }
            const QFileInfoList l2 = q1.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QFileInfo &d2 : l2) {
                QDir q2(d2.absoluteFilePath());
                if (q2.exists("model.rknn"))
                    modelDirs << q2.absolutePath();
            }
        }
    }
    if (modelDirs.isEmpty()) {
        QDir(tmp).removeRecursively();
        QMessageBox::warning(this, "未找到模型",
            "zip 内未找到含 model.rknn 的模型目录");
        return;
    }

    QStringList okList, dupList, failList;
    for (const QString &dirPath : modelDirs) {
        QString id = QFileInfo(dirPath).fileName().toLower();
        id.replace(' ', '-');
        const QString srcLabel = QFile::exists(dirPath + "/labels.txt")
            ? dirPath + "/labels.txt" : QString();
        QString err;
        if (ModelRegistry::instance().importModel(dirPath + "/model.rknn", srcLabel, id, err)) {
            (err.isEmpty() ? okList : dupList) << (err.isEmpty() ? id : err);
        } else {
            failList << QString("%1: %2").arg(id, err);
        }
    }
    QDir(tmp).removeRecursively();
    refreshModelLibraryTable();
    refreshModelParamsTable();
    reloadEquipmentService();

    QString summary = QString("共 %1 个模型目录\n新导入: %2\n跳过(重复): %3")
        .arg(modelDirs.size())
        .arg(okList.isEmpty() ? "无" : okList.join(", "))
        .arg(dupList.isEmpty() ? "无" : QStringList(dupList).join("; "));
    if (!failList.isEmpty())
        summary += "\n失败:\n" + failList.join("\n");
    QMessageBox::information(this, "zip 导入结果", summary);
    log("model", QString("zip 模型包导入: 成功 %1, 重复 %2, 失败 %3")
            .arg(okList.size()).arg(dupList.size()).arg(failList.size()));
}

// ============================================================
// deleteSelectedModel: 删除表格选中模型（registry 内做引用检查）
//   registry.deleteModel 移入 model/.trash/，非物理删除
// ============================================================
void frmMain::deleteSelectedModel()
{
    if (!modelLibTable_) return;
    const int row = modelLibTable_->currentRow();
    if (row < 0 || !modelLibTable_->item(row, 0)) {
        QMessageBox::information(this, "删除模型", "请先在列表中选中一行模型");
        return;
    }
    const QString id = modelLibTable_->item(row, 0)->text();
    if (QtHelper::askConfirm(this, QStringLiteral("删除模型"),
                             QStringLiteral("确认把模型 [%1] 移出模型库？\n目录将移入 model/.trash/，可人工恢复。")
                                 .arg(id))
        != QMessageBox::Ok) {
        return;
    }

    QString err;
    if (ModelRegistry::instance().deleteModel(id, err)) {
        refreshModelLibraryTable();
        refreshModelParamsTable();
        reloadEquipmentService();
        log("model", QString("模型库移除: %1").arg(id));
    } else {
        QMessageBox::warning(this, "删除被拒", err);
    }
}

// ============================================================
// testSelectedModel: 对选中模型做单帧推理验证（P2 验证工具）
//   现场构造 YOLO11Model（NPU 加载约数秒，期间 GUI 短暂阻塞，
//   验证工具可接受）→ detect(assets/test/test.jpg) → 报检出数与明细；
//   函数退出即析构，release_yolo11_model 归还 NPU 资源
// ============================================================
void frmMain::testSelectedModel()
{
    if (!modelLibTable_) return;
    const int row = modelLibTable_->currentRow();
    if (row < 0 || !modelLibTable_->item(row, 0)) {
        QMessageBox::information(this, "模型测试", "请先在列表中选中一行模型");
        return;
    }
    const QString id = modelLibTable_->item(row, 0)->text();
    ModelRegistry &reg = ModelRegistry::instance();
    const ModelMeta m = reg.meta(id);
    if (m.id.isEmpty()) {
        QMessageBox::warning(this, "模型测试", "模型不在库中: " + id);
        return;
    }
    const QString testImage = RuntimePaths::assetsTestImage();
    if (!QFileInfo::exists(testImage)) {
        QMessageBox::warning(this, "模型测试", "测试图片不存在: " + testImage);
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString summary;
    int detCount = 0;
    try {
        YOLO11Model model(reg.modelPath(id).toStdString(),
                          reg.labelPath(id).toStdString(),
                          RKNN_NPU_CORE_AUTO,
                          m.numClasses > 0 ? m.numClasses : 80);

        image_buffer_t img{};
        const int rc = read_image(testImage.toUtf8().constData(), &img);
        if (rc != 0 || img.virt_addr == NULL) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, "模型测试", "测试图片解码失败: " + testImage);
            return;
        }

        object_detect_result_list results;
        // 阈值按该库 id 走同一条回落链：这里若还用 detect() 的形参默认值，
        // 现场就会看到"测试页框一堆、监控页一个不见"，然后怀疑设置没生效。
        const float testConf = ModelRegistry::resolveConfThreshold(id);
        const float testNms = ModelRegistry::resolveNmsThreshold();
        model.detect(&img, &results, false, testConf, testNms);   // infer 内部先 memset 再填充
        free(img.virt_addr);            // read_image 用 malloc/stbi 分配，成对释放

        const std::vector<std::string> &names = model.getClassNames();
        detCount = results.count;
        QStringList lines;
        lines << QString("模型: %1").arg(id)
              << QString("测试图: %1 (%2x%3)").arg(testImage).arg(img.width).arg(img.height)
              << QString("阈值: conf %1 / nms %2").arg(testConf, 0, 'f', 3).arg(testNms, 0, 'f', 3)
              << QString("检出目标: %1 个").arg(results.count);
        for (int i = 0; i < results.count && i < 15; ++i) {
            const object_detect_result &r = results.results[i];
            const QString cls = (r.cls_id >= 0 && r.cls_id < (int)names.size())
                ? QString::fromStdString(names[r.cls_id]) : QString("?");
            lines << QString("  %1  %2%  [%3,%4 - %5,%6]")
                         .arg(cls)
                         .arg(r.prop * 100.0, 0, 'f', 1)
                         .arg(r.box.left).arg(r.box.top)
                         .arg(r.box.right).arg(r.box.bottom);
        }
        if (results.count > 15)
            lines << QString("  ...（仅列前 15 个）");
        summary = lines.join("\n");
    } catch (const std::exception &e) {
        QApplication::restoreOverrideCursor();
        QMessageBox::warning(this, "模型加载失败", QString::fromUtf8(e.what()));
        log("model", QString("模型测试失败 %1: %2").arg(id).arg(e.what()));
        return;
    }
    QApplication::restoreOverrideCursor();
    QMessageBox::information(this, "模型测试结果", summary);
    log("model", QString("模型测试 %1: 检出 %2 个目标").arg(id).arg(detCount));
}

// ==========================================
// 视频通道控制
// ==========================================

/**
 * @brief 打开视频到指定通道
 * @param channel 通道索引（0-3，对应 videoWindow1~4）
 * @param path    视频文件路径或RTSP地址
 */
void frmMain::openVideoToChannel(int channel, const QString &path)
{
    if (!videoWindow) return;

    // 走 frmVideoWindow::openVideo 这一条口径：它会带上通道号。
    // 直接调 PlayerWidget::open(path) 会用默认参数 channel=0，把这一路的
    // decoder 通道号改成 0——槽位解析、报警键、围栏全部按通道 0 走。
    videoWindow->openVideo(channel, path);
    if (!path.isEmpty())
        log("system", QString("通道%1 视频源已加载: %2（模型与报警类别沿用当前配置，改动在设置页即时生效）")
                        .arg(channel + 1).arg(path));
}

// ==========================================
// 4路视频浏览按钮
// ==========================================

/**
 * @brief 浏览某一路的视频源并立即打开
 * @param channel 通道号（0-based，与 config 的 video.channels 下标同口径）
 * @param edit    该通道源地址所在的 QLineEdit
 *
 * 选中文件后：更新输入框 → 写 config 并落盘 → openVideoToChannel 重开这一路。
 */
void frmMain::browseChannelSource(int channel, QLineEdit *edit)
{
    if (!edit) return;
    const QString currentPath = edit->text().isEmpty()
        ? QDir::homePath()
        : QFileInfo(edit->text()).path();

    QFileDialog dialog(this, QString("选择通道%1视频文件").arg(channel + 1));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        const QString file = dialog.selectedFiles().first();
        edit->setText(file);
        ConfigManager::instance().setChannelSource(channel, file);
        ConfigManager::instance().save();
        openVideoToChannel(channel, file);
    }
}

void frmMain::on_btnBrowseCh1_clicked() { browseChannelSource(0, ui->lineEditCh1); }
void frmMain::on_btnBrowseCh2_clicked() { browseChannelSource(1, ui->lineEditCh2); }
void frmMain::on_btnBrowseCh3_clicked() { browseChannelSource(2, ui->lineEditCh3); }
void frmMain::on_btnBrowseCh4_clicked() { browseChannelSource(3, ui->lineEditCh4); }

// ==========================================
// 阈值变化 -> 保存配置 + 日志
// ==========================================

/**
 * @brief 置信度阈值变化槽（全局档 = 回落链的底座）
 * 值没变就直接返回：save() 是持锁把整份 JSON 截断重写，而 valueChanged 是**每一步**
 * 触发一次（触屏上长按 ▼ 会连发）。隔壁通道地址编辑器同样是这个防抖口径。
 * 生效范围是所有"跟随全局"的槽位，所以推给全部通道，改动不需要重开流。
 */
void frmMain::on_spinBoxConfThresh_valueChanged(double value)
{
    ConfigManager &cfg = ConfigManager::instance();
    if (qFuzzyCompare(cfg.confThreshold(), value))
        return;
    cfg.setConfThreshold(value);
    cfg.save();
    log("system", QString("置信度阈值已更新: %1").arg(cfg.confThreshold(), 0, 'f', 2));
    applyGlobalThresholdNow();
}

/**
 * @brief NMS 阈值变化槽
 * @brief 同上：只有全局一档（NMS 是跨模型的 IoU 合并口径，每模型再配一列不值）。
 */
void frmMain::on_spinBoxNmsThresh_valueChanged(double value)
{
    ConfigManager &cfg = ConfigManager::instance();
    if (qFuzzyCompare(cfg.nmsThreshold(), value))
        return;
    cfg.setNmsThreshold(value);
    cfg.save();
    log("system", QString("NMS阈值已更新: %1").arg(cfg.nmsThreshold(), 0, 'f', 2));
    applyGlobalThresholdNow();
}

// ==========================================
// 公共日志接口
// ==========================================

/**
 * @brief 追加日志到文本框（简单文本，无颜色）
 * @param msg 日志消息
 */
void frmMain::appendLog(const QString &msg)
{
    ui->textBrowserLog->append(msg);
}
