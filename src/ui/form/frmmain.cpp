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
#include "rtsp_live_service.h"    // 带检测框视频的 RTSP 直播分发（§6.1 liveUrl 优先指它）
#include "pad_ws_server.h"        // Pad 接口 v1 的入站 WS 服务端（本机是服务端；装配点在此）
#include "pad_wire.h"             // defaultDeviceId()：HTTP 面的 device_id 与 WS 那条同源
#include "pad_http_server.h"      // Pad 接口 v1 的入站 HTTP `/api/` 服务端（§4~§7；装配点在此）
#include "pad_http_api.h"         // 按配置装配那四条章节的路由
#include "roll_call_widget.h"
#include "equipment_inventory_widget.h"
#include "smart_recognition_widget.h"
#include "login_widget.h"        // 扫码登录页（stackedWidget 第 0 页 = 门）
#include "construction_session.h"
#include "construction_detail_dialog.h"  // 点右上角登录人弹出的施工详情
#include "stream_editor_dialog.h"        // 视频流注册表（video.sources[]）编辑对话框
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

/**
 * @brief 构造主窗（顺序约束见 frmmain.h 的 @brief）
 *
 * 五个 init* 调用之间的依赖：initForm 第 9 步才 load 配置，后面每一步都在读它；
 * initNewPages 里两处按**索引** insertWidget(1/2) 插看板与报警列表，initBusinessPages
 * 是尾追加智能识别页 —— 两者对调不会让功能坏（切页一律按控件指针），但页序会变。
 * enableTouchScrollAll 与 showLoginGate 必须排在最末：前者只扫"此刻已存在"的子件，
 * 后者要求所有页面都已建好才盖得住。
 * 四路视频不在构造里开：原第 11 步已挪到 onLoginSucceeded() → startAllStreams()。
 */
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

/**
 * @brief 析构只做 delete ui（停流已在 closeEvent 完成，见 frmmain.h 的说明）
 *
 * delete ui 会连带拆掉对象树里的业务页与两个宿主 QDialog；如果那时还有模态识别框
 * 在跑，它的析构里 quit()+wait() 会等 NPU 收尾（那类对话框都是 exec() 模态，
 * 正常流程走不到这里，所以不必在这里额外做停线程处理）。
 * 成员里的 shared_ptr 服务在本函数返回后才析构，顺序上晚于所有页面 —— 页面析构
 * 期间服务仍然存活，这是它们敢于在析构里回调服务的原因。
 */
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
 *
 * 只认 **7 个字符的 #RRGGBB**（mid 的长度写死 7）：皮肤层里所有标记都按这个格式写。
 * 遇到 rgba(...)、#RRGGBBAA 或 6 位不带 # 的写法会截出错的串而**不报错**；
 * 找不到 flag 时 color 保持调用方传进来的原值（不清空），所以调用前要自己给默认色。
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

    // 9.5 装入扫码登录的校验口径（qr-login-edge-v1.md §6.6）：只有"本机当前计划"这一条。
    //     §6.4 有效期与 §6.5 验签本轮不做（2026-11-04 裁定）。装在登录页出现之前，
    //     否则第一张码会按"不比对"放过去。
    {
        session::LoginPolicy policy;
        policy.expectedTaskId = ConfigManager::instance().qrLoginExpectedTaskId();
        session::ConstructionSession::instance().setLoginPolicy(policy);
    }

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
 *
 * ⚠ 两处按字符位置硬取色，都依赖 blacksoft.css 的**头部写法不变**：
 *   - qss.mid(20, 7) 取全局调色板底色（跳过文件开头那条注释行的固定长度）；
 *   - getQssColor() 从标记后再截 7 个字符（只认 #RRGGBB）。
 * CSS 一改版（换行、加 BOM、改成 rgba）这两处就会静默取到错色，不会报错也不会崩——
 * 长期方向是改用 theme:: 常量（本目录新写的控件都走那边），这两个取值只剩
 * 给老导航栏（initLeftMain 那条链）用。
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
    // NOTE: darkTextColor 取的是 normalTextColor（不是上面任何 darkColorStart/End），
    // 于是这两项恒等。qtemplate 模板遗留写法，唯一的消费者是 initLeftMain 的
    // IconHelper::StyleColor；要改配色请改 theme:: 常量，别在这里"顺手修成 dark*"，
    // 那会让那条已经没人走的左侧导航链跟着变颜色。
    this->borderColor = highColor;
    this->normalBgColor = normalColorStart;
    this->darkBgColor = panelColor;
    this->normalTextColor = textColor;
    this->darkTextColor = normalTextColor;
}

/**
 * @brief 装配动态页面 + 集中建立全部跨域信号连接（本类的"接线板"）
 *
 * 干的事：注册跨线程元类型 → 顶部导航改文本（去 icon，字号 FS_NAV_TOUCH）→
 * 动态插页（0=登录门、1=看板、2=报警列表，末尾追加智能识别页）→
 * 报警 toast/角标/面板节流 → AlarmManager / WsClient / FenceManager / AlarmUplink /
 * PadWsServer 的连接与服务启动 → 首次刷角标。
 *
 * 时序与线程要点：
 *   - 必须在 initForm()（已 load 配置）之后调用：导航文本、看板与报警列表都要读配置；
 *   - 页面切换一律按**控件指针**（setCurrentWidget），插页导致索引漂移也不怕；
 *   - AlarmManager::alarmGenerated 由**解码线程**发出，这里所有 lambda 都经自动
 *     队列连接回到 GUI 线程才跑，所以可以安全动 ui->；
 *   - 报警面板节流的 lambda 引用的 alarmRefreshTimer_ 要到本函数后半段才 new：
 *     同一趟 GUI 线程里信号不会在 connect 与 new 之间派发，所以成立。
 *     **别把这段拆成两次调用**，拆了就变成"先连接、后建对象"的真悬垂；
 *   - btnExit 在这里只改文本，分派靠对象身份（见 buttonClick）：按文字匹配的链
 *     一旦文本改了就会静默失配（点了没反应，最难查的一类坏法）。
 *
 * 副作用：启动 WsClient（配置里有地址就会一直重连）、按需启动内嵌 HTTP 文件服务
 * 与 Pad 服务端。后两者的 stop 在 src/main.cpp 的事件循环之后，与本函数刻意不成对
 * （mk_env_init 是进程级一次，停了不能再起）。
 */
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

    // Pad 接口 v1 的入站服务端（本机是 WS 服务端，Pad 连进来）。**默认开**（2026-11-04 起），
    // 打开它等于在局域网里开一个明文零鉴权的收报警的口 —— 唯一的门是握手 Origin 白名单。
    // 日志出口无条件连一次：服务端是单例，调试设置页随时可能把它关掉再起起来，
    // 只在 enabled 分支里连就会让"页面上刚开的服务"一条日志都不打。
    connect(&pad::PadWsServer::instance(), &pad::PadWsServer::logMessage,
            this, [this](const QString &cat, const QString &msg) {
                log(cat, msg);
            });
    // HTTP 面的日志走同一个出口（category 固定 "pad-http"）。这条 connect 放在 enabled
    // 判断**之外**，理由与上面那句一样：调试页里当场打开的服务端不该一条日志都不打。
    connect(&pad::PadHttpServer::instance(), &pad::PadHttpServer::logMessage,
            this, [this](const QString &cat, const QString &msg) {
                log(cat, msg);
            });
    if (uplinkCfg.padServerEnabled()) {
        QString padErr;
        if (!startPadServerFromConfig(&padErr)) {
            log("warning", QStringLiteral("Pad 服务端没起来：%1").arg(padErr));
        } else {
            log("system", QStringLiteral("Pad 服务端已监听 ws://0.0.0.0:%1%2（明文零鉴权，"
                                         "局域网内任何设备都能连进来收报警）")
                              .arg(uplinkCfg.padServerPort()).arg(uplinkCfg.padServerPath()));
        }
    }

    // Pad 接口 v1 的 HTTP `/api/` 面（§4~§7）。与上面那台 WS 是**两个口、两台服务端**：
    // 那台只推报警，这台能被写——处置/误报/已读/围栏增删改全从这里进。
    // 默认开着（pad_http.enabled 出厂 true），关掉是彻底不 bind。
    if (uplinkCfg.padHttpEnabled()) {
        QString httpErr;
        if (!startPadHttpFromConfig(&httpErr)) {
            log("warning", QStringLiteral("Pad HTTP 面没起来：%1").arg(httpErr));
        } else {
            log("system", QStringLiteral("Pad HTTP 面已监听 http://0.0.0.0:%1/api/（明文零鉴权，"
                                         "局域网内任何设备都能改报警状态与围栏；关闭请写 "
                                         "pad_http.enabled=false）")
                              .arg(uplinkCfg.padHttpPort()));
            if (!uplinkCfg.padHttpExposeStreamCredentials()) {
                // 只在默认那档提一次：现场看不到"为什么 Pad 播不起来"的时候，这一句是唯一线索
                log("info", QStringLiteral("发给 Pad 的 liveUrl 已剥掉视频源口令"
                                          "（要保留请打开 pad_http.expose_stream_credentials）"));
            }
        }
    }

    // Pad RTSP 直播（带检测框画面）：本机把画好框的码流经 ZLM 分发，
    // §6.1 的 liveUrl 优先指到这里。默认开（pad_rtsp.enabled 出厂 true）。
    // 与 HttpFileService 共享 mk_env_init1（zlm_env），谁先起谁初始化运行时。
    if (uplinkCfg.padRtspEnabled()) {
        const quint16 rtspPort = static_cast<quint16>(uplinkCfg.padRtspPort());
        if (!RtspLiveService::instance().start(rtspPort)) {
            log("warning", QStringLiteral("Pad RTSP 直播没起来 port=%1，"
                                          "发给 Pad 的 liveUrl 将回落到原始视频源地址").arg(rtspPort));
        } else {
            log("system", QStringLiteral("Pad RTSP 直播已监听 rtsp://0.0.0.0:%1/live/<CHx>"
                                         "（带检测框画面；关闭请写 pad_rtsp.enabled=false）")
                              .arg(rtspPort));
        }
    }

    // 类别名表由解码器按通道槽位建立（见 FFmpegVideoDecoder::buildCascadeTasks），
    // 这里不再从全局标签文件填 AlarmManager。

    updateAlarmBadge();
}

/**
 * @brief 按当前配置起 Pad WS 服务端（连同它出证据用的 8090 文件服务）
 * @param err 失败原因（中文），可为空
 * @return true = 已在监听
 *
 * 只负责**起**：调用方要先 PadWsServer::stop() 再调它（调试页"保存并应用"就是这么做的），
 * 因为 start() 在已监听时会直接返回 true、不会换成新端口。
 * 证据服务是进程级一次性的（mk_env_init 起、之后不能再起），所以这里只在没跑时补起，
 * 任何情况下都不会停它；配置写了 media_base_url 时连起都不起，直接按配置的地址发。
 */
bool frmMain::startPadServerFromConfig(QString *err)
{
    ConfigManager &cfg = ConfigManager::instance();
    pad::ServerConfig padCfg;
    padCfg.port = static_cast<quint16>(cfg.padServerPort());
    padCfg.path = cfg.padServerPath();
    padCfg.deviceId = cfg.padServerDeviceId();
    padCfg.syncLimit = cfg.padServerSyncLimit();

    const QString mediaOverride = cfg.padServerMediaBaseUrl();
    HttpFileService &media = HttpFileService::instance();
    const QString mediaRoot = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
    if (mediaOverride.isEmpty() && !media.running()) {
        const quint16 mediaPort = static_cast<quint16>(cfg.alarmUplinkMediaHttpPort());
        if (!media.start(mediaRoot, mediaPort)) {
            log("warning", QStringLiteral("Pad 服务端要出证据地址，但媒体文件服务 port=%1 没起来"
                                          "——本次报警只发文字").arg(mediaPort));
        }
    }
    padCfg.mediaRootDir = mediaRoot;
    padCfg.mediaBaseUrl = media.baseUrl(mediaOverride);

    return pad::PadWsServer::instance().start(padCfg, err);
}

/**
 * @brief 按当前配置起 Pad HTTP `/api/` 服务端（v1 §4 处置 / §5 围栏 / §6 视频 / §7 设备）
 * @param err 失败原因（中文），可为空
 * @return true = 已在监听
 *
 * 与 startPadServerFromConfig 的三点不同，都是"这台服务端"自己的事：
 *   1) **路由在这里装**（registerPadHttpRoutes 先 clearRoutes 再逐条 addRoute）。
 *      ctx 按值进各个 handler，所以改了配置必须重新调本函数才生效——这正是调试页
 *      「保存并应用」存在的理由。
 *   2) **依赖同一个 8090 媒体服务**：围栏画面与整改照片的 URL 都由它出字节。
 *      这里只在它没起来时补起一次，从不停它（进程级一次性，见 http_file_service.h）。
 *      补不起就照样监听——§4/§5/§6/§7 里除了 frameUrl/photoUrls 都不依赖媒体服务，
 *      为了一个画面地址把整个 `/api/` 关掉是过度的。
 *   3) **deviceId 与 WS 那条同源**（pad::defaultDeviceId()）：两个接口给对端报两个设备号
 *      是比报错更难查的问题，所以两边都读 pad_server.device_id 这一个键。
 *
 * 先 stop 再起：PadHttpServer::start() 在已监听时幂等返回 true，不换口。
 */
bool frmMain::startPadHttpFromConfig(QString *err)
{
    ConfigManager &cfg = ConfigManager::instance();

    pad::HttpServerConfig httpCfg;
    httpCfg.port = static_cast<quint16>(cfg.padHttpPort());
    // 正文上限按 MB 配、传进去是字节：26MB 那档的来由（§4.2 五张 5MB + 余量）写在
    // ConfigManager::padHttpMaxBodyMb 与 pad_http_server.h 的默认值上，两处一致。
    httpCfg.maxBodyBytes = static_cast<qint64>(cfg.padHttpMaxBodyMb()) * 1024LL * 1024LL;
    httpCfg.deviceId = cfg.padServerDeviceId().isEmpty()
        ? pad::defaultDeviceId()
        : cfg.padServerDeviceId();

    pad::HttpApiContext ctx;
    ctx.deviceId = httpCfg.deviceId;
    ctx.exposeStreamCredentials = cfg.padHttpExposeStreamCredentials();
    // 通道在线判据接到 AlarmManager 那张表上：§6 的 status 与 §7.1 的 cameras[].online
    // 必须同源，两处各读各的就会出现"视频页说在线、设备页说离线"。
    ctx.channelOnline = [](int channel) {
        return AlarmManager::instance().isChannelOnline(channel);
    };
    ctx.fenceFrameDir = cfg.padHttpFenceFrames() ? QStringLiteral("fence-frames") : QString();

    const QString mediaOverride = cfg.padServerMediaBaseUrl();
    HttpFileService &media = HttpFileService::instance();
    const QString mediaRoot = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath();
    if (mediaOverride.isEmpty() && !media.running()) {
        const quint16 mediaPort = static_cast<quint16>(cfg.alarmUplinkMediaHttpPort());
        if (!media.start(mediaRoot, mediaPort)) {
            log("warning", QStringLiteral("Pad HTTP 面要出围栏画面/整改照片地址，但媒体文件服务"
                                          " port=%1 没起来——apply 会回 503/3003，处置照片不给地址")
                               .arg(mediaPort));
        }
    }
    ctx.mediaRootDir = mediaRoot;
    ctx.mediaBaseUrl = media.baseUrl(mediaOverride);

    pad::PadHttpServer &server = pad::PadHttpServer::instance();
    pad::registerPadHttpRoutes(&server, ctx);
    return server.start(httpCfg, err);
}

/**
 * @brief 显示一条悬浮报警提示（右下角 toast）
 * @param alarm 报警记录；channel 是 0-based，文案里 +1 显示
 *
 * 线程：槽经队列连接在 GUI 线程执行（发方是解码线程的 storeAndNotify）。
 * 措辞分三种口径是**有意的**，别合并成一条模板：
 *   - 围栏 vs 类别："检测到 person"会让现场以为围栏那几路的 COCO 也在按类别报警，
 *     而那个模型的 alarm_class 明明为空（只画框不报警）；
 *   - 围栏内部再分闯入禁区 / 离岗（alarmType == "fence_out"）：都写成"围栏侵入"
 *     就会把人往反方向找；
 *   - 负责人不上 toast：它是会话级常量，同一批报警每条都一样，占位不信息。
 * 副作用与限制：toast 是 this 的子件、盖在整窗之上，adjustSize + move 每次重算，
 * 位置按**当前窗口尺寸**取右下角（全屏切换后下一条报警自然会重定位，不用额外处理）；
 * 连发多条只显示最后一条（3s 单发定时器被反复重置）；alarmTime 解析失败时
 * QDateTime::fromString 返回非法值，时间段会显示为空而不报错。
 */
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
    // 负责人不再出现在 toast 上：它是会话级常量（同一批报警全都一样），
    // 而这一页的口径是"列表与提示只看事件本身，人/计划进详情看"。
    alarmToast_->setText(QString("%1 (%2%%)  %3")
        .arg(head)
        .arg(alarm.confidence * 100, 0, 'f', 1)
        .arg(dt.toString("HH:mm:ss")));
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

/**
 * @brief 刷新导航「报警数据」按钮上的未确认角标
 *
 * 数据源是 AlarmManager::unacknowledgedCount()（纯内存窗口内的计数，不是 DB 总数）。
 * 边界与取舍：
 *   - >99 一律显示 99：角标 min-width 24 / max-width 48，三位数会把钮撑破并把字挤走；
 *   - 定位每次都用 btnEquipCheck->width() 重算（不是写死坐标）：导航钮从"图上字下"
 *     改成纯文字后文字矩形上移，写死就会压字（README 里"探针看不见这种重叠"那条）；
 *   - 0 条时整颗 hide，而不是显示 "0"——留着会让人以为"有报警但没内容"。
 * 调用点有四处：statsUpdated 信号、VideoAlarmWidget::alarmUpdated（面板刷新一并带上）、
 * 新报警到达（showAlarmToast 末尾）、initNewPages 末尾的首次刷新。
 */
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

/**
 * @brief 处理对端（Aibox 服务端）经 WebSocket 下发的一条报警
 * @param data 报警 JSON：alarm_id / alarm_type / timestamp / image_url / video_url
 *
 * 当前实现**只打日志 + 回 ack**：不入 AlarmManager、不入库、不弹 toast、不动角标。
 * 理由：下行报警与本机检测报警是两条独立链路，混进同一个列表会让现场分不清
 * "这条是我这几路看到的，还是对端推给我的"。要真展示得先决定归属与去重口径。
 * 边界：
 *   - alarm_id 为空就不发 ack（对端按 id 记账，空 id 回了也没意义）；
 *   - timestamp 只读不用（Q_UNUSED）——它是 double 转 long 的秒/毫秒口径未确认值，
 *     按它排序之前得先跟服务端对齐量纲，别猜。
 */
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

/**
 * @brief 弹出设备盘点任务页（窗口级模态）
 *
 * 与 showRollCallDialog() 同一条口径：先 refreshTaskList() 再 show()，
 * 因为任务可能被点名侧改动（两个服务共用同一张 task 表）。
 * 用 show()+WindowModal 而不是 exec()：点名/盘点的详情页是**非模态顶层窗口**，
 * 应用级模态会把它一起锁死。判空针对的是"initBusinessPages() 还没走到/中途失败"，
 * 正常情况下宿主对话框一定存在（服务失败时卡片直接置灰、点不动，见 initBusinessPages）。
 */
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

/**
 * @brief 创建设备盘点服务并加载模型
 * @return false = 模型库里既没有 yolo11n-coco 也没有明火模型（调用方弹警告、卡片置灰）
 *
 * ⚠ 必须在 initRollCallService() **之后**调用：构造时把 rollCallService_ 交给它，
 *   两者共用 roll_call.db 的同一张 task 表（盘点任务 type="equipment_registration"），
 *   task_id 两侧可互用。点名服务为空时这里**不判空**也不报错——服务层明许可传空
 *   （见 equipment_inventory_service.h 构造注释：此时所有库操作静默返回默认值），
 *   真正致命的只有"模型清单为空"这一条，所以只检它。
 * 失败时 equipmentService_ 仍是一个"存在但未 ready"的实例，页面按 isReady() 置灰创建钮。
 */
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
 *
 * NOTE: **当前整条链不产生任何可见效果**。btnsMain 从未被填过（.ui 里唯一的候选容器
 * widgetLeftMain 只装了一个 verticalSpacer，且 maximumWidth 锁死 10px，frmmain.ui:408），
 * 所以下面那个 for 一次都不进、IconHelper::setStyle() 收到的是空列表；
 * iconsMain 的三个 glyph 也没有落点。这三个 glyph 与"左侧导航"的描述属模板遗留文案，
 * 别按它去找界面上不存在的按钮。要恢复左侧图标导航得先在 .ui 里给 widgetLeftMain 加件，
 * 或在代码里建按钮后 append 进 btnsMain（本目录约定不手改 .ui，所以走后者）。
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

/**
 * @brief 左侧导航按钮互斥选中（只改 checked 状态，不切页面）
 * NOTE: 随 btnsMain 一起失效（见 initLeftMain），循环体一次都不进。
 * 它与顶部导航的 buttonClick() 是两条独立的链：本函数只负责"选中态"，
 * 不动 stackedWidget —— 重建左侧导航时别顺手把切页逻辑接到这里。
 */
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

/**
 * @brief 标题栏 × = 退出整个程序（不是退出登录）
 *
 * 走 systemExit() 的确认框，因此登录页上也按得到它（widgetMenu 在合门时**不收起**，
 * 与登录页那颗「退出程序」同义、不冲突）。与导航「退出登录」（只回登录页、进程不死）
 * 是两件事，改这两处文案时别把它们合并。
 */
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

/**
 * @brief 开门：按当前配置把该开的通道拉起来（幂等）
 *
 * 调用点只有 onLoginSucceeded()；与 stopAllStreams() 成对，守卫 streamsRunning_。
 * 前提：ConfigManager::load() 已跑过（initForm 第 9 步），所以本函数**不能**挪到
 * initForm 之前，也不能在登录页被提前调。
 * 失败/跳过场景（都不算错误、都不弹提示，只体现在日志与四宫格占位上）：
 *   - channelEnabled(ch)==false → 整路不开；
 *   - resolveChannelSource(ch) 解析不出地址（未配置，或 source_id 悬空）→ 不开，
 *     悬空告警由 openVideo 那侧统一打，避免两处各说一套。
 * 通道数这里用 cfg.channelCount()，其余处（停流、状态回调连接、围栏）写死 4：
 * 两者当前等价（channelCount() 恒返回 4，与 .ui 里四个 PlayerWidget 同数）。
 * 真要扩路数，瓶颈是 .ui 的四宫格而不是这个常量。
 * ⚠ 本函数在 GUI 线程逐个 openVideo（解码器内部起线程，不阻塞等待）。
 *   日志里"已开流 N 路"只代表**已提交**：FFmpegVideoDecoder::start() 是 void，
 *   上一轮 stop 超时遗弃了解码线程时它会拒绝重启（liveLoops_>0，只 emit error），
 *   这里无从分辨。表现为该路仍挂着占位文案，去设置页动一下本路的启用/选流
 *   （走 applyChannelConfigNow）即可重试。
 */
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
        // 按**解析后的流**开这一路：内联地址的旧配置、以及引用 video.sources[] 的新配置
        // 都在 resolveChannelSource 里合流，这里不再自己拼一遍判据。
        // 解析不出地址（未配置，或 source_id 悬空）就不开：空地址交给 openVideo 拦，
        // 悬空 id 的告警也在那一步打，界面留"视频待接入"而不是黑一块。
        const VideoSourceCfg source = cfg.resolveChannelSource(ch);
        if (source.url.isEmpty())
            continue;
        videoWindow->openVideo(ch, source);
        ++opened;
    }
    streamsRunning_ = true;
    log("system", QString("登录后可用，已开流 %1 路（共 %2 路）")
                      .arg(opened).arg(cfg.channelCount()));
}

/**
 * @brief 停看板刷新 + 两阶段停四路解码（退出登录与关窗共用同一段）
 *
 * 线程：只在 GUI 线程调，且**会阻塞整个界面**直到四路真退出（单路有界：
 * 3s+10s+1.5s×槽位，驱动卡住时更久）。因此不能挂在报警/帧回调路径上。
 * 幂等：streamsRunning_ 为 false 时直接返回，closeEvent 与退出登录连着调两次
 * 也只停一次（重复停虽无害，但会打出误导性的第二遍日志）。
 * 顺序：先 dashboardWidget_->stop()——看板每 2s 重建一次并读 AlarmManager，
 * 流停掉后报警还在收尾时继续刷会看到半套数据；下一次切到看板页由它自己的
 * showEvent 重启，这里不需要显式 start()。
 * 上限写死 4（不是 cfg.channelCount()）：这四路是 .ui 里的四个 PlayerWidget，
 * 与配置侧的通道数当前等价。
 */
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

/**
 * @brief 合门：显示登录页、收起顶部导航、清掉登录态显示
 *
 * 调用点两处：构造的**最后一步**（门必须盖住所有已建好的页面）与退出登录后。
 * login_ 为空直接返回——它是在 initNewPages() 里建的，构造顺序若被打乱（比如有人
 * 把合门提前到 initForm 末尾），这里就会静默什么都不做，表现是"启动直接进主页"。
 * resetStatus() 不能省：否则新来的班次看到的是上一班的"缺字段"红字。
 * 只收 widgetTop、不收 widgetMenu：× 就是退出程序，与登录页那颗按钮同义、不冲突。
 */
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

/**
 * @brief 按当前会话刷新标题栏右上角的登录人角标
 *
 * 只在 GUI 线程调（动控件）。合门/开门时都要**单独调它**：sessionBadge_ 是
 * widgetTitle 的直接子件，不在被 hide() 的那块 widgetTop 里面，showLoginGate()
 * 收不到它；漏掉这一句，登录页右上角就还挂着一颗"登录人 …"，看着像仍登着。
 * 文本只放负责人姓名一项（其余字段点进 ConstructionDetailDialog 看）；未登录是
 * **整颗 hide**而不是清空文字——留一颗空钮在右上角会被当成"能点的按钮"。
 * 这一颗是全机唯一的常驻登录态显示——报警归属在 makeAlarmRecord() 就定死了，
 * 换班没点「退出登录」会继续记到上一班名下，只有一行会滚走的日志可看不够。
 */
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
    // 只放姓名：这一颗回答的是"现在系统记的是谁"，施工id/电话/工号点开来在详情框里
    // 全都有。带上"登录人 …·施工id"那阵子，18px 的字在 1920 宽里跟导航行抢横向空间，
    // 三字姓名加个 id 就开始截字 —— 截了字就等于没有这个提示。
    sessionBadge_->setText(session::ConstructionInfo::display(info.personName));
    sessionBadge_->show();
    sessionBadge_->raise();
}

/**
 * @brief 点登录人角标 → 弹出当前施工信息详情框
 *
 * dialog 是栈上对象，本函数返回即析构（不需要 deleteLater，也没有残留窗口）。
 * ⚠ 「退出登录」不能由详情框自己直接调 onLogoutRequested()：那样就是两层模态
 * ——登出确认框会被压在详情框下面，岗位上按不到。所以详情框只置一个
 * logoutRequested 标志，由这里在 exec() **返回之后**再走登出。
 * 前提：本函数只在已登录时被触发（角标未登录是隐藏的）。
 */
void frmMain::openSessionDetail()
{
    ConstructionDetailDialog dialog(this);
    dialog.exec();
    // 登出必须等这个框先下去再走：onLogoutRequested 自己还要弹一个确认框，
    // 在详情框的模态循环里调它就是两层模态，确认框会被压在详情框下面、岗位上按不到。
    if (dialog.logoutRequested()) {
        onLogoutRequested();
    }
}

/**
 * @brief 开门：登录成功（扫码或测试登录同一条路径）后的恢复动作
 *
 * 由 LoginWidget::loginSucceeded 触发（GUI 线程；扫码器也在 GUI 线程的模态框里跑）。
 * 顺序：先亮回 widgetTop → 刷登录人角标 → 切到视频监控页 → startAllStreams()。
 *   切页在开流之前，报警面板因此先就位：alarmGenerated 的节流 lambda 里有
 *   "当前页 == pageMonitor 才刷面板"的判据，反过来就会把开门最初几秒的报警
 *   只留在 DB 里（切页时 buttonClick() 会补一次 refreshAlarms，所以不是丢数据，
 *   只是晚一步显示）。
 * 这里用 ui->btnMain->click() 而不是直接 setCurrentWidget：走按钮既有分派才顺带
 * 把 setChecked 归位和右侧报警面板刷新一起做掉（与启动时同一条路径，少一套逻辑）。
 * 幂等由 startAllStreams() 自己保证，重复信号不会开第二遍流。
 */
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
    // 登录后切到该施工计划的围栏（DB 里按 taskId 存的那批）；
    // 未登录时内存里没有围栏，登录后才加载并驱动检测。
    geofence::FenceManager::instance().reloadForTask(
        session::ConstructionSession::instance().snapshot().taskId);
    log("system", QString("扫码登录成功：%1")
                      .arg(session::ConstructionSession::instance().summary()));
}

/**
 * @brief 导航「退出登录」：确认 → 停流 → 清限流窗口 → 登出 → 合门
 *
 * 进程**不死**，只回到登录页（真正的退出程序是 × 与登录页那颗按钮，走 systemExit）。
 * 清什么：四路解码、AlarmManager 的限流内存 map、会话、上一班的 toast 与角标显示。
 * 不清什么（都是有意保留，别"顺手加clean"）：
 *   - DB 历史与 data/alarms 文件——归属在 makeAlarmRecord() 就写死了，属于上一班；
 *   - 围栏 DB 数据（native_fences）——仍按 taskId 留在库里，下次同任务登录可恢复；
 *     但**内存围栏要清空**（reloadForTask("")），未登录不再驱动检测；
 *   - WsClient 连接与**待发上行队列**——施工信息在报警产生时就定死了，发送时不再
 *     回查会话，所以重传的那条仍带着它产生时的归属，清空反而会丢上报。
 * resetThrottles() 不是可选的：不清的话重登后 2s（类别）/围栏窗口期内的第一条
 * 报警会被静默吞掉，界面一条都不出（现象像"重新登录就再也不报警了"）。
 * 副作用：stopAllStreams() 阻塞 GUI 数秒；确认框必须按主窗所在屏居中（双屏口径同
 * systemExit()，弹到没触摸的那块屏等于把人锁在原地）。
 */
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
    // 登出后清空内存围栏：未登录不驱动检测，但 DB 里的历史仍在、同 task 再登可恢复。
    geofence::FenceManager::instance().reloadForTask(QString());

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

/**
 * @brief 关窗：先停四路，再退事件循环
 *
 * ⚠ 顺序不能倒：stopAllStreams() 必须在 qApp->quit() **之前**跑完，
 *   这样 delete ui 拆 PlayerWidget 时四条解码线程都已退出（QThread 还在跑就被
 *   销毁会直接 abort）。event->accept() 放在停流之后也是有意的：GUI 线程这期间
 *   阻塞数秒，用户看到的"卡一下"是等解码退出，不是关窗没生效。
 * 未登录时这里等于空转（streamsRunning_ 为 false）。
 * 注：析构 ~frmMain() 不再负责停流，见那边的说明。
 */
void frmMain::closeEvent(QCloseEvent *event)
{
    // 停流序列抽到 stopAllStreams()（与"退出登录"共用同一段，见那边的说明）
    stopAllStreams();
    event->accept();
    qApp->quit();
}

/**
 * @brief 空实现（.ui 对象名自动连接的遗留槽，当前不会被触发）
 * NOTE: pageRoll 没有设 contextMenuPolicy，Qt 只在 CustomContextMenu 下才发
 *   customContextMenuRequested，所以这条连接永不触发；触屏也没有右键可弹上下文菜单。
 *   pageRoll 这一页本身也不再挂载业务内容（点名已改为智能识别卡片 + 弹窗）。
 *   保留只为不动 .ui；要清理就把 .ui 的 pageRoll 与这条声明一起摘掉。
 */
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
 *   - 检测配置：全局置信度/NMS 阈值（真开关：随帧透传到 detect()，改完即时生效，
 *     每模型还可在模型参数表里覆盖置信度）
 *
 * 同时输出初始日志到运行日志面板
 */
// ── 设置页三个"内容区"的可视档位 ─────────────────────────────
// 为什么必须写**最小**高度：pageSettings 整体包在 QScrollArea(widgetResizable) 里，
// 内容区只会被撑到"各块最小高度之和"（板端实测：内容 minimumSizeHint=1177 时布局就
// 只给 1177，谁的下限小谁被压扁），没有下限的表格/日志会塌成表头那一截，表现就是
// "模型参数/模型库/运行日志整块折叠、看不见内容"。上限只用来防止模型多了把页面撑得没边。
// ⚠ 这个下限还得躲开 QSS：任何一条命中该控件的 min-height 规则都会在 polish 时**改写**
//   minimumSize（不是取大），代码里设的值会被顶掉 —— 见 theme::panel() 的说明。
// kTableHeaderH 是实测档（16px 字 + ::section 的 4px 上下 padding），差几像素无妨：
// 这里要的是"至少能看清 N 行"，不是像素级对齐。
constexpr int kTableHeaderH = 48;
constexpr int kModelTableMinRows = 5;   ///< 两张模型表至少露 5 行（约 330px）
constexpr int kModelTableMaxRows = 8;   ///< 再多就靠页内滚动，不把别的块顶出屏
constexpr int kLogMinLines = 12;        ///< 运行日志至少 12 行（现场靠它排障）
constexpr int kLogLineH = 24;           ///< 16px 字的行高

// 取设置页滚动区的内容布局（定义在本文件后面，initDebugPage 要先用一次）
static QVBoxLayout *settingsContentLayout(QWidget *page);

/// 上面的说明块讲"装什么"，这里补一条"怎么插"：本函数第一件事就是把 pageSettings 的
/// 原控件整体搬进 QScrollArea，此后**所有**分组框都必须经 settingsContentLayout()
/// 插进滚动内容里。直接 addWidget 到 pageSettings 自己的布局 = 钉在滚动区之外
/// （不参与滚动、白占一屏高度），还会打破"第 0 项是滚动区"这个前提，
/// 后面每个分组框都只 qWarning 一句就静默加不进来（看着像功能消失）。
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

    // 运行日志至少 12 行。这个下限**只能写在它自己的样式表里**：皮肤层
    // `QTextEdit{min-height:42px;padding:6px 10px;border:1px}` 对 QSS 是"声明即赋值"，
    // initStyle() 重新 polish 时会把 setMinimumHeight(288) 直接改写成 42+12+2=56，
    // 于是日志塌成两三条、整页看起来像叠在一起。组件自身样式表优先级高于皮肤层，
    // 274 = 288 − 竖向 padding 12 − 上下边框 2（padding/边框来自皮肤层，不在这里重复声明）。
    ui->textBrowserLog->setStyleSheet(
        QString("QTextBrowser { min-height:%1px; }").arg(kLogMinLines * kLogLineH - 14));

    // ========== 视频通道（config 的 video.channels，下标即通道号） ==========
    // 模型路径/标签文件/输入尺寸/模型类型四行在本轮 schema 里失去意义：
    // config 只写模型库目录名，路径与标签由 model/library 解析，所以整行隐藏。
    for (int i = 0; i < 4; ++i) {
        // 四颗源地址手填框整片隐藏：选流由下拉负责（下拉就放在它们原来那一格）。
        // 先 removeWidget 再 hide：留在网格里的那一格会同时被两颗控件占着，
        // 单元格的尺寸要按两者一起算，几何上不如干脆摘干净。
        QLineEdit *chEdit = channelSourceEdit(i);
        ui->gridLayout_model->removeWidget(chEdit);
        chEdit->setVisible(false);
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
        grp->setTitle("视频通道（选流 / 备注 / 启用 / 级联模型）");

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

    // ========== 每路：选流下拉 + 备注 + 启用开关 + 级联模型 + 清空 ==========
    {
        QGridLayout *gl = ui->gridLayout_model;   // 现有通道所在的分组框网格
        for (int i = 0; i < 4; ++i) {
            // 选流下拉占在手填框原来那一格（那四颗框已摘出布局并隐藏，见上面）。
            // 为什么是下拉而不是输入框：一条流的参数有十项（类型/尺寸/帧率/循环/超时/重连…），
            // 输入框只给得出 url，其余必然全部回落默认——那注册表就白建了。
            QComboBox *sourceCombo = new QComboBox;
            sourceCombo->setMinimumWidth(300);   // 与原手填框同宽，整行几何不动
            theme::makeComboTouch(sourceCombo);  // 弹层行高抬到触屏档，否则选项要点歪
            chSourceCombo_[i] = sourceCombo;
            gl->addWidget(sourceCombo, i, 1);
            connect(sourceCombo, QOverload<int>::of(&QComboBox::activated), this,
                    [this, i]() { applyChannelSourceChoice(i); });

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

            // 清空按钮：撤掉本路的流引用 + 内联地址 + 备注，并立刻停本路
            QPushButton *clearBtn = new QPushButton("清空");
            clearBtn->setMinimumWidth(88);
            clearBtn->setStyleSheet(theme::mainButton(theme::DANGER));
            connect(clearBtn, &QPushButton::clicked, this, [this, i]() {
                chNoteEdit_[i]->clear();
                ConfigManager &c = ConfigManager::instance();
                // 引用与内联两个键一起清：setChannelSource("") 内部已顺带清 source_id，
                // 这里再写一次是不让"清空"这个动作依赖那条实现细节。
                c.setChannelSourceId(i, QString());
                c.setChannelSource(i, "");
                c.setChannelNote(i, "");
                c.save();
                log("system", QString("通道%1 视频流引用与备注已清空").arg(i + 1));
                refreshChannelSourceCombos();
                // 清完就重开本路：只改配置不停流，画面还在跑刚才那条引用，
                // 现场会以为"清空没生效"。
                applyChannelConfigNow(i);
            });
            gl->addWidget(clearBtn, i, 6);
        }

        // 视频流注册表入口：一条流可以被多路同时引用，它不属于任何一行通道，
        // 所以单独占一行放在四路下面（跨"标签 + 下拉"两列，不动既有列宽）。
        QPushButton *streamMgrBtn = new QPushButton("视频流管理…");
        streamMgrBtn->setMinimumWidth(140);
        streamMgrBtn->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        connect(streamMgrBtn, &QPushButton::clicked, this, [this]() { openStreamManager(); });
        gl->addWidget(streamMgrBtn, gl->rowCount(), 0, 1, 2);

        // 灌数据放在四颗下拉都建好之后：refresh 会按当前 config 把每路选中项摆对，
        // 期间全程屏蔽信号，不会在这里就触发一次重开流。
        refreshChannelSourceCombos();
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

    // ========== 报警上报 WebSocket（本机连出去） ==========
    {
        QGroupBox *grpWs = new QGroupBox("报警上报 WebSocket（本机连出去）");
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

        // 放进滚动区的内容布局里，和围栏分组同一个写法。
        // 原来这里插的是 pageSettings 自己的布局：那段代码在开头已经把页内控件整体搬进
        // QScrollArea，该布局只剩滚动区一项，insertWidget(count-1=0, grpWs) 等于把这块
        // 钉在滚动区**之外**的页面顶部——它不参与滚动、白占一屏高度，还把
        // "滚动区是第 0 项"这个前提打破，后面任何分组框都会静默加不进来。
        QVBoxLayout *contentLayout = settingsContentLayout(ui->pageSettings);
        if (contentLayout) {
            contentLayout->insertWidget(contentLayout->count() - 1, grpWs);
        } else {
            qWarning() << "initDebugPage: 找不到设置页滚动区，WebSocket 分组未加入";
            grpWs->deleteLater();
        }
    }

    // ========== Pad 服务端（WS 入站 8900） ==========
    // 与上面那个分组不是一回事：上面是**本机连出去**的旧链路（WsClient），
    // 这里是 Pad 连进来收报警的 v1 服务端（pad_server）。改完点「保存并应用」= 写配置 +
    // 停服再起服（QWebSocketServer 可以重开，端口换了必须重启监听器才生效；
    // 已连的 Pad 会被断开，它自己会重连）。
    {
        QGroupBox *grpPad = new QGroupBox("Pad 服务端（WS 入站，本机推报警给 Pad）");
        grpPad->setStyleSheet(QString("QGroupBox{font-size:%1px; font-weight:bold;}"
                                      "QLabel{font-size:%2px;}"
                                      "QLineEdit{font-size:%2px;}"
                                      "QSpinBox{font-size:%2px;}"
                                      "QPushButton{font-size:%2px;}")
                                  .arg(theme::FS_CARD)
                                  .arg(theme::FS_BODY));
        QGridLayout *glPad = new QGridLayout(grpPad);
        glPad->setHorizontalSpacing(10);
        glPad->setVerticalSpacing(6);

        auto *chkEnabled = new QCheckBox("启用 Pad WS 服务端");
        chkEnabled->setChecked(cfg.padServerEnabled());
        glPad->addWidget(chkEnabled, 0, 0, 1, 2);

        auto *labStatus = new QLabel;
        glPad->addWidget(labStatus, 0, 2, 1, 2);

        auto *spinPort = new QSpinBox;
        spinPort->setRange(1024, 65535);
        spinPort->setValue(cfg.padServerPort());
        glPad->addWidget(new QLabel("端口:"), 1, 0);
        glPad->addWidget(spinPort, 1, 1);
        auto *labPortHint = new QLabel("本口：Pad 连进来收报警推送（ws://本机IP:端口）");
        labPortHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glPad->addWidget(labPortHint, 1, 2, 1, 2);

        auto *editPath = new QLineEdit(cfg.padServerPath());
        editPath->setPlaceholderText("/");
        glPad->addWidget(new QLabel("WS 路径:"), 2, 0);
        glPad->addWidget(editPath, 2, 1);
        auto *labPathHint = new QLabel("只进日志，不用于筛连接");
        labPathHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glPad->addWidget(labPathHint, 2, 2, 1, 2);

        auto *editDevice = new QLineEdit(cfg.padServerDeviceId());
        editDevice->setPlaceholderText("空 = 按 /etc/machine-id 派生");
        glPad->addWidget(new QLabel("设备 ID:"), 3, 0);
        glPad->addWidget(editDevice, 3, 1, 1, 3);

        auto *editMedia = new QLineEdit(cfg.padServerMediaBaseUrl());
        editMedia->setPlaceholderText("空 = 自动：本机对外 IP + 媒体端口（默认 8090）");
        glPad->addWidget(new QLabel("媒体地址前缀:"), 4, 0);
        glPad->addWidget(editMedia, 4, 1);
        auto *labMediaHint = new QLabel("图片、片段、处置照片的下载地址都由它拼");
        labMediaHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glPad->addWidget(labMediaHint, 4, 2, 1, 2);

        auto *spinSync = new QSpinBox;
        spinSync->setRange(1, 5000);
        spinSync->setValue(cfg.padServerSyncLimit());
        glPad->addWidget(new QLabel("单次同步上限:"), 5, 0);
        glPad->addWidget(spinSync, 5, 1);
        auto *labSyncHint = new QLabel("一次 sync_result 最多几条");
        labSyncHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glPad->addWidget(labSyncHint, 5, 2, 1, 2);

        auto *btnApply = new QPushButton("保存并应用");
        btnApply->setMinimumWidth(120);
        btnApply->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        glPad->addWidget(btnApply, 6, 3);

        auto refreshStatus = [labStatus, chkEnabled]() {
            const bool running = pad::PadWsServer::instance().running();
            if (!chkEnabled->isChecked()) {
                labStatus->setText("○ 已停用");
                labStatus->setStyleSheet("color: #dc3232;");
            } else if (running) {
                labStatus->setText(QString("● 监听中 0.0.0.0:%1")
                                       .arg(ConfigManager::instance().padServerPort()));
                labStatus->setStyleSheet("color: #32b432;");
            } else {
                labStatus->setText("✕ 未监听");
                labStatus->setStyleSheet("color: #dc3232;");
            }
        };
        refreshStatus();

        connect(btnApply, &QPushButton::clicked, this,
                [this, chkEnabled, spinPort, editPath, editDevice, editMedia, spinSync,
                 refreshStatus]() {
                    ConfigManager &c = ConfigManager::instance();
                    c.setPadServerEnabled(chkEnabled->isChecked());
                    c.setPadServerPort(spinPort->value());
                    c.setPadServerPath(editPath->text());
                    c.setPadServerDeviceId(editDevice->text());
                    c.setPadServerMediaBaseUrl(editMedia->text());
                    c.setPadServerSyncLimit(spinSync->value());
                    c.save();
                    // 先停再起：start() 在已监听时是幂等的（直接返回 true），
                    // 不停一下换端口就等于没换。已连的 Pad 会被断开，它会自己重连。
                    pad::PadWsServer::instance().stop();
                    QString err;
                    bool ok = false;
                    if (chkEnabled->isChecked()) {
                        ok = startPadServerFromConfig(&err);
                    }
                    refreshStatus();
                    if (!chkEnabled->isChecked()) {
                        log("system", "Pad 服务端已停用（配置已保存）");
                    } else if (ok) {
                        log("system", QString("Pad 服务端已按新配置监听 0.0.0.0:%1%2")
                                          .arg(c.padServerPort()).arg(c.padServerPath()));
                    } else {
                        log("warning", QString("Pad 服务端没起来：%1").arg(err));
                    }
                });

        QVBoxLayout *padContent = settingsContentLayout(ui->pageSettings);
        if (padContent) {
            padContent->insertWidget(padContent->count() - 1, grpPad);
        } else {
            qWarning() << "initDebugPage: 找不到设置页滚动区，Pad 服务端分组未加入";
            grpPad->deleteLater();
        }
    }

    // ========== Pad HTTP 服务端（/api/ 入站 8899，v1 §4~§7） ==========
    // 与上面那一组是**两个口、两台服务端**：那台只往外推报警，这台能被写
    // （处置/误报/已读 + 围栏建/改/下发/停用/删草稿）。所以这一组多出来的两个开关
    // 都是围绕"写面"的：正文上限（处置带 0~5 张照片）与围栏画面渲染。
    // 「保存并应用」= 写配置 + 存盘 + 停服再装路由起服（QTcpServer 与 QWebSocketServer
    // 一样可以重开；路由里 ctx 是按值捕获的，不重装就还是旧配置）。
    {
        QGroupBox *grpHttp = new QGroupBox("Pad HTTP 服务端（/api/ 入站，Pad 写入处置与围栏）");
        grpHttp->setStyleSheet(QString("QGroupBox{font-size:%1px; font-weight:bold;}"
                                       "QLabel{font-size:%2px;}"
                                       "QCheckBox{font-size:%2px;}"
                                       "QSpinBox{font-size:%2px;}"
                                       "QPushButton{font-size:%2px;}")
                                   .arg(theme::FS_CARD)
                                   .arg(theme::FS_BODY));
        QGridLayout *glHttp = new QGridLayout(grpHttp);
        glHttp->setHorizontalSpacing(10);
        glHttp->setVerticalSpacing(6);

        auto *chkHttpEnabled = new QCheckBox("启用 Pad HTTP /api/ 面（明文零鉴权，可写）");
        chkHttpEnabled->setChecked(cfg.padHttpEnabled());
        glHttp->addWidget(chkHttpEnabled, 0, 0, 1, 2);

        auto *labHttpStatus = new QLabel;
        glHttp->addWidget(labHttpStatus, 0, 2, 1, 2);

        auto *spinHttpPort = new QSpinBox;
        spinHttpPort->setRange(1024, 65535);
        spinHttpPort->setValue(cfg.padHttpPort());
        glHttp->addWidget(new QLabel("端口:"), 1, 0);
        glHttp->addWidget(spinHttpPort, 1, 1);
        auto *labHttpPortHint = new QLabel("本口：Pad 写入接口（http://本机IP:端口/api/）");
        labHttpPortHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glHttp->addWidget(labHttpPortHint, 1, 2, 1, 2);

        auto *spinBody = new QSpinBox;
        spinBody->setRange(1, 100);
        spinBody->setValue(cfg.padHttpMaxBodyMb());
        glHttp->addWidget(new QLabel("正文上限(MB):"), 2, 0);
        glHttp->addWidget(spinBody, 2, 1);
        auto *labBodyHint = new QLabel("处置提交最多带 5 张照片，每张 5MB，再加余量");
        labBodyHint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        glHttp->addWidget(labBodyHint, 2, 2, 1, 2);

        auto *chkFenceFrame = new QCheckBox("围栏下发时渲染一张生效画面 PNG，给 Pad 一个可点开的地址");
        chkFenceFrame->setChecked(cfg.padHttpFenceFrames());
        glHttp->addWidget(chkFenceFrame, 3, 0, 1, 4);

        auto *chkExposeCred = new QCheckBox("liveUrl 保留视频源口令（默认剥掉）");
        chkExposeCred->setChecked(cfg.padHttpExposeStreamCredentials());
        glHttp->addWidget(chkExposeCred, 4, 0, 1, 4);

        auto *btnHttpApply = new QPushButton("保存并应用");
        btnHttpApply->setMinimumWidth(120);
        btnHttpApply->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
        glHttp->addWidget(btnHttpApply, 5, 3);

        auto refreshHttpStatus = [labHttpStatus, chkHttpEnabled]() {
            const bool running = pad::PadHttpServer::instance().running();
            if (!chkHttpEnabled->isChecked()) {
                labHttpStatus->setText("○ 已停用");
                labHttpStatus->setStyleSheet("color: #dc3232;");
            } else if (running) {
                labHttpStatus->setText(QString("● 监听中 0.0.0.0:%1（%2 条路由，已连 %3）")
                                           .arg(ConfigManager::instance().padHttpPort())
                                           .arg(pad::PadHttpServer::instance().routeCount())
                                           .arg(pad::PadHttpServer::instance().clientCount()));
                labHttpStatus->setStyleSheet("color: #32b432;");
            } else {
                labHttpStatus->setText("✕ 未监听");
                labHttpStatus->setStyleSheet("color: #dc3232;");
            }
        };
        refreshHttpStatus();

        connect(btnHttpApply, &QPushButton::clicked, this,
                [this, chkHttpEnabled, spinHttpPort, spinBody, chkFenceFrame, chkExposeCred,
                 refreshHttpStatus]() {
                    ConfigManager &c = ConfigManager::instance();
                    c.setPadHttpEnabled(chkHttpEnabled->isChecked());
                    c.setPadHttpPort(spinHttpPort->value());
                    c.setPadHttpMaxBodyMb(spinBody->value());
                    c.setPadHttpFenceFrames(chkFenceFrame->isChecked());
                    c.setPadHttpExposeStreamCredentials(chkExposeCred->isChecked());
                    c.save();
                    // 先停再起：start() 在已监听时幂等返回 true，不停一下换口等于没换。
                    // 停的顺序也在装路由之前——registerPadHttpRoutes 只换表不影响监听，
                    // 但放在 stop 之后起，读到的就是"这一套配置 + 这一张路由表"。
                    pad::PadHttpServer::instance().stop();
                    QString err;
                    bool ok = false;
                    if (chkHttpEnabled->isChecked())
                        ok = startPadHttpFromConfig(&err);
                    refreshHttpStatus();
                    if (!chkHttpEnabled->isChecked()) {
                        log("system", "Pad HTTP 面已停用（配置已保存，端口不再监听）");
                    } else if (ok) {
                        log("system", QString("Pad HTTP 面已按新配置监听 0.0.0.0:%1/api/（路由已重装）")
                                          .arg(c.padHttpPort()));
                    } else {
                        log("warning", QString("Pad HTTP 面没起来：%1").arg(err));
                    }
                });

        QVBoxLayout *httpContent = settingsContentLayout(ui->pageSettings);
        if (httpContent) {
            httpContent->insertWidget(httpContent->count() - 1, grpHttp);
        } else {
            qWarning() << "initDebugPage: 找不到设置页滚动区，Pad HTTP 面分组未加入";
            grpHttp->deleteLater();
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
//   这四颗框现在**已摘出布局并隐藏、不再读写**：选流由下拉负责（见 refreshChannelSourceCombos），
//   内联 source 那个键的值直接从 ConfigManager 读（channelInlineSource）。
//   控件本身留在 .ui 里不动 —— 摘出布局 + 隐藏就够了，不必改 Designer 里的对象名。
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

// 下拉每一项的两个角色：kind 决定"选中它意味着什么动作"，id 只在 kind=流 时有意义。
// 用角色而不是靠文本前缀判断：文本是给人看的，会随显示口径改，动作判据不能跟着漂。
enum SourcePickKind {
    kPickStream = 0,   ///< 引用注册表里的一条流（id 见 kSrcRoleStreamId）
    kPickInline = 1,   ///< 用通道里内联的 source（旧口径）
    kPickNone = 2,     ///< 不选流：这一路不开
    kPickBroken = 3,   ///< 配置里的 source_id 在注册表里已不存在（悬空引用，只展示不可选动作）
};
static const int kSrcRoleKind = Qt::UserRole;
static const int kSrcRoleStreamId = Qt::UserRole + 1;

// ============================================================
// refreshChannelSourceCombos: 按当前 config 重灌四路"选流"下拉
// ============================================================
// 全程 blockSignals：这个函数只在"界面要跟配置对齐"时调用（建页、注册表改动、
// 清空），任何一次都不该被当成用户动作去写盘或重开流。
// ============================================================
void frmMain::refreshChannelSourceCombos()
{
    ConfigManager &cfg = ConfigManager::instance();
    const QList<VideoSourceCfg> streams = cfg.videoSources();
    for (int i = 0; i < 4; ++i) {
        QComboBox *combo = chSourceCombo_[i];
        if (!combo)
            continue;   // 设置页还没建起来（登录门在它之前），这里无事可做
        const QString curId = cfg.channelSourceId(i);
        const QString inlineUrl = cfg.channelInlineSource(i);
        combo->blockSignals(true);
        combo->clear();

        for (const VideoSourceCfg &s : streams) {
            // 显示 "id · 备注(没有备注就显示地址)"：id 是唯一键，必须打头，
            // 否则两条备注相同的流在下拉里长得一样。完整参数留在 tooltip。
            combo->addItem(QStringLiteral("%1 · %2")
                               .arg(s.id, s.note.isEmpty() ? s.url : s.note), kPickStream);
            const int row = combo->count() - 1;
            combo->setItemData(row, s.id, kSrcRoleStreamId);
            combo->setItemData(row,
                               QStringLiteral("地址：%1\n类型：%2 ｜ 尺寸：%3 ｜ 帧率：%4\n循环：%5 ｜ 硬解：%6 ｜ 超时：%7ms ｜ 重连：%8ms")
                                   .arg(s.url,
                                        s.type.isEmpty() ? QStringLiteral("(按地址自动识别)") : s.type)
                                   .arg(s.width > 0 && s.height > 0
                                            ? QStringLiteral("%1x%2").arg(s.width).arg(s.height)
                                            : QStringLiteral("(不指定)"))
                                   .arg(s.fps > 0 ? QString::number(s.fps) : QStringLiteral("(不指定)"))
                                   .arg(s.loop ? QStringLiteral("开") : QStringLiteral("关"))
                                   .arg(s.hardwareDecode ? QStringLiteral("开") : QStringLiteral("关"))
                                   .arg(s.timeoutMs)
                                   .arg(s.reconnectMs),
                               Qt::ToolTipRole);
        }

        if (!curId.isEmpty() && cfg.videoSource(curId).url.isEmpty()) {
            // 悬空引用要**显式列出来**而不是悄悄不选中：这一路不开流，界面却看不出为什么，
            // 现场就会去查摄像头而不是查配置（ConfigManager 那边的口径是"宁可不黑屏也要留告警"）。
            combo->addItem(QStringLiteral("（流 %1 已不存在）").arg(curId), kPickBroken);
            combo->setItemData(combo->count() - 1, curId, kSrcRoleStreamId);
        }
        if (!inlineUrl.isEmpty())
            combo->addItem(QStringLiteral("（内联地址）%1").arg(inlineUrl), kPickInline);
        if (curId.isEmpty() && inlineUrl.isEmpty())
            combo->addItem(QStringLiteral("（不选流，本路不开）"), kPickNone);

        int sel = -1;
        for (int r = 0; r < combo->count(); ++r) {
            const int kind = combo->itemData(r, kSrcRoleKind).toInt();
            if (kind == kPickInline) {
                if (curId.isEmpty()) { sel = r; break; }   // 没引用注册表 → 当前就在用内联
                continue;
            }
            if (combo->itemData(r, kSrcRoleStreamId).toString() == curId) { sel = r; break; }
        }
        combo->setCurrentIndex(sel < 0 ? 0 : sel);
        combo->blockSignals(false);
    }
}

// ============================================================
// applyChannelSourceChoice: 用户在某一路的下拉里选了别的项
// ============================================================
// activated 在"重新点中当前项"时也会发，所以每个分支都先比值再动：
// 不判就会把"点了没改"变成一次停流 + 重开流。
// ============================================================
void frmMain::applyChannelSourceChoice(int channel)
{
    QComboBox *combo = chSourceCombo_[channel];
    if (!combo || combo->currentIndex() < 0)
        return;
    const int kind = combo->currentData(kSrcRoleKind).toInt();
    if (kind == kPickBroken)
        return;   // 只是把悬空状态显示出来，没有"切到悬空"这个动作

    ConfigManager &c = ConfigManager::instance();
    if (kind == kPickStream) {
        const QString id = combo->currentData(kSrcRoleStreamId).toString();
        if (c.channelSourceId(channel) == id)
            return;
        c.setChannelSourceId(channel, id);
        c.save();
        log("system", QStringLiteral("通道%1 已引用视频流 %2：%3")
                        .arg(channel + 1)
                        .arg(id, c.videoSource(id).url));
    } else {
        // 内联 / 不选流 都是"撤掉引用"，区别只在 config 里还留没留内联地址
        if (c.channelSourceId(channel).isEmpty())
            return;
        c.setChannelSourceId(channel, QString());
        c.save();
        log("system", kind == kPickInline
                ? QStringLiteral("通道%1 已改用内联地址：%2")
                      .arg(channel + 1).arg(c.channelInlineSource(channel))
                : QStringLiteral("通道%1 已不引用任何流，本路不开").arg(channel + 1));
    }
    applyChannelConfigNow(channel);
}

// ============================================================
// openStreamManager: 打开"视频流管理"对话框，落盘后重开受影响的那几路
// ============================================================
void frmMain::openStreamManager()
{
    ConfigManager &cfg = ConfigManager::instance();
    // 先记住每路当前引用的是哪条流：对话框里删一条流时，ConfigManager 会把引用它的
    // source_id 一起清掉，事后再拿"当前引用"去比对就找不到受影响的那一路了。
    QString before[4];
    for (int i = 0; i < 4; ++i)
        before[i] = cfg.channelSourceId(i);

    StreamEditorDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;   // 取消：改的是工作副本，注册表一个字节都没动

    const QStringList touched = dlg.touchedSourceIds();
    refreshChannelSourceCombos();
    if (touched.isEmpty())
        return;
    for (int i = 0; i < 4; ++i) {
        // 引用变了（被删流连带清空）或引用的那条流被改过 → 整路重开
        if (before[i] != cfg.channelSourceId(i) || touched.contains(before[i]))
            applyChannelConfigNow(i);
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
    // 解析整条流（不只是地址）：这一路重开时要带走的是 type/尺寸/帧率/loop/超时/重连，
    // 只取 url 的话注册表里改这些参数就得重启才生效——而"改了不生效"正是本仓最忌讳的假开关。
    const VideoSourceCfg source = cfg.resolveChannelSource(channel);
    const QStringList ids = cfg.channelModelIds(channel);
    if (!cfg.channelEnabled(channel))
        log("system", QStringLiteral("通道%1 已停用，本路不开流").arg(channel + 1));
    else if (source.url.isEmpty())
        log("system", QStringLiteral("通道%1 没有可用的视频流（未配置，或 source_id 悬空），本路不开流")
                          .arg(channel + 1));
    else {
        videoWindow->openVideo(channel, source);
        log("system", QStringLiteral("通道%1 已重开: %2%3 ｜ 模型 %4")
                            .arg(channel + 1)
                            .arg(source.url)
                            .arg(source.id.isEmpty()
                                     ? QString()
                                     : QStringLiteral("（流 %1）").arg(source.id))
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
    if (!pageLayout)
        return nullptr;
    // 遍历找那个 QScrollArea，而不是认定它在第 0 项：曾经有一处把 WebSocket 分组框
    // insertWidget(0, ...) 插到了页面布局里，于是"第 0 项是滚动区"这个前提一破，
    // 所有后续分组框都只打一条 qWarning 就静默不加入页面（看着像功能消失）。
    // 顺序不该决定控件在不在。
    for (int i = 0; i < pageLayout->count(); ++i) {
        QScrollArea *sa = qobject_cast<QScrollArea *>(pageLayout->itemAt(i)->widget());
        if (sa)
            return qobject_cast<QVBoxLayout *>(sa->widget()->layout());
    }
    return nullptr;
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
    group->setStyleSheet(theme::panel(theme::PANEL));
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
    // 行高 56 后按"表头 48 + N 行"重算上下限，否则可见行数会被行长挤没
    modelParamTable_->setMinimumHeight(kTableHeaderH + kModelTableMinRows * theme::TOUCH_TABLE_ROW);
    modelParamTable_->setMaximumHeight(kTableHeaderH + kModelTableMaxRows * theme::TOUCH_TABLE_ROW);
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
    group->setStyleSheet(theme::panel(theme::PANEL));
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
    // 1..600 秒（10 分钟）是本目录给的可选档。⚠ 已知不一致：手改 JSON 写出范围外的值
    // （例如 5000）时，setValue() 会被 QSpinBox **静默夹到 600**，而 connect 在这行之后，
    // 所以 config 里仍是 5000、界面显示 600 —— 判定端读的是 config，表现就是
    // "界面上 10 分钟、实际半小时才报一次"。要支持更宽的窗口得先抬这个上限。
    throttleSpin->setRange(1, 600);
    throttleSpin->setValue(cfg.geofenceThrottleSeconds());
    throttleSpin->setStyleSheet(theme::field(theme::PANEL));
    gl->addWidget(throttleSpin, 1, 1);

    // NOTE: 这两条链与设置页其余下拉的口径**不一样**：其余连 activated（只有用户动作才发），
    // 这里连的是 currentTextChanged / valueChanged，程序化灌值同样会发。
    // 当前成立的前提是"所有 addItems/setCurrentText/setValue 都排在 connect 之前"。
    // 以后若要在本函数后半段（或模型库变更后）重灌一次候选，必须 blockSignals 包住，
    // 否则每次刷新都白写一次盘（ConfigManager::save() 是持锁整份 JSON 截断重写、非原子），
    // 2026-10-31 板上就是按 mtime 抓出过这类重复写盘。
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
    group->setStyleSheet(theme::panel(theme::PANEL));
    QVBoxLayout *v = new QVBoxLayout(group);

    QHBoxLayout *bar = new QHBoxLayout();
    QPushButton *btnRefresh = new QPushButton("刷新");
    QPushButton *btnImport = new QPushButton("导入模型...");
    QPushButton *btnZip = new QPushButton("导入zip包...");
    QPushButton *btnTest = new QPushButton("测试选中");
    QPushButton *btnUpdate = new QPushButton("更新选中模型...");
    QPushButton *btnRollback = new QPushButton("版本/回滚...");
    QPushButton *btnDelete = new QPushButton("删除选中");
    btnRefresh->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnImport->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnZip->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnTest->setStyleSheet(theme::mainButton(theme::SUCCESS));
    btnUpdate->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnRollback->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    btnDelete->setStyleSheet(theme::mainButton(theme::DANGER));
    QLabel *hint = new QLabel("库目录 model/library；历史版本在 data/model_versions；改选路径后重启程序生效");
    hint->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    bar->addWidget(btnRefresh);
    bar->addWidget(btnImport);
    bar->addWidget(btnZip);
    bar->addWidget(btnTest);
    bar->addWidget(btnUpdate);
    bar->addWidget(btnRollback);
    bar->addWidget(btnDelete);
    bar->addStretch();
    bar->addWidget(hint);
    v->addLayout(bar);

    modelLibTable_ = new QTableWidget(0, 7);
    modelLibTable_->setHorizontalHeaderLabels(
        {"id", "名称", "任务", "类别数", "输入", "被引用", "版本/历史"});
    modelLibTable_->verticalHeader()->setVisible(false);
    modelLibTable_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    modelLibTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    modelLibTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    modelLibTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    modelLibTable_->setShowGrid(false);
    modelLibTable_->horizontalHeader()->setStretchLastSection(false);
    modelLibTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    // 行高 56 后按"表头 48 + N 行"重算上下限，否则可见行数会被行长挤没
    modelLibTable_->setMinimumHeight(kTableHeaderH + kModelTableMinRows * theme::TOUCH_TABLE_ROW);
    modelLibTable_->setMaximumHeight(kTableHeaderH + kModelTableMaxRows * theme::TOUCH_TABLE_ROW);
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
    connect(btnUpdate, &QPushButton::clicked, this, &frmMain::updateSelectedModel);
    connect(btnRollback, &QPushButton::clicked, this, &frmMain::rollbackSelectedModel);
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
            // 版本号 + 已归档的历史条数。括号里是 0 表示这份权重是"第一代"，
            // 还没被更新过；不是 0 就能点「版本/回滚」挑回去。
            QStringLiteral("%1（历史 %2）").arg(m.version.isEmpty() ? QStringLiteral("—")
                                                                       : m.version)
                                           .arg(reg.archivedVersions(id).size()),
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
// updateSelectedModel: 换同一 id 的权重（旧的那份归档，不丢）
// ============================================================
// 现场迭代模型时 id 不变（通道的模型列表、参数表都按 id 引用），改的是那一份 .rknn。
// 以前只能"删除 → 重新导入"，而删除被引用守卫挡着，等于要先把四个通道的引用拆掉；
// 归档在 data/model_versions/<id>/<版本号>/，所以删错了/换坏了还能滚回来。
void frmMain::updateSelectedModel()
{
    if (!modelLibTable_) return;
    const int row = modelLibTable_->currentRow();
    if (row < 0 || !modelLibTable_->item(row, 0)) {
        QMessageBox::information(this, "更新模型", "请先在列表中选中一行模型");
        return;
    }
    const QString id = modelLibTable_->item(row, 0)->text();
    ModelRegistry &reg = ModelRegistry::instance();
    const ModelMeta m = reg.meta(id);
    if (m.id.isEmpty()) {
        QMessageBox::warning(this, "更新模型", "模型不在库中: " + id);
        return;
    }

    QString rknn = QFileDialog::getOpenFileName(
        this, QStringLiteral("更新模型 [%1]: 选择新的 .rknn 文件").arg(id),
        "model", "RKNN 模型 (*.rknn)");
    if (rknn.isEmpty()) return;

    const QString labels = QFileDialog::getOpenFileName(
        this, QStringLiteral("更新模型 [%1]: 选择标签文件（可取消=沿用原标签）").arg(id),
        "model", "文本文件 (*.txt);;所有文件 (*)");

    const int users = reg.referenceCount(id);
    if (QtHelper::askConfirm(
            this, QStringLiteral("更新模型"),
            QStringLiteral("确认用新权重替换 [%1]？\n"
                           "现役那份（版本 %2）会归档到 data/model_versions/%1/，可回滚。\n"
                           "本模型被 %3 个通道引用，替换后这些通道会重新拉流。")
                .arg(id)
                .arg(m.version.isEmpty() ? QStringLiteral("—") : m.version)
                .arg(users))
        != QMessageBox::Ok) {
        return;
    }

    QString err;
    if (!reg.updateModel(id, rknn, labels, err)) {
        QMessageBox::warning(this, "更新失败", err);
        log("model", QStringLiteral("模型库更新失败: %1 %2").arg(id, err));
        return;
    }
    // 权重换了必须重建 NPU 上下文：applyModelParamsNow 只换报警类别与阈值，
    // 用它的话"版本列显示新版、画面还是旧模型的输出"，是最难归因的一种不一致。
    const QList<int> channels = ConfigManager::instance().modelParamUsers(id);
    for (int ch : channels)
        applyChannelConfigNow(ch);
    refreshModelLibraryTable();
    refreshModelParamsTable();
    reloadEquipmentService();
    QMessageBox::information(this, "更新模型",
                             err.isEmpty()
                                 ? QStringLiteral("已更新 [%1]，引用通道 %2 路已重开").arg(id)
                                       .arg(channels.size())
                                 : err);
    log("model", QStringLiteral("模型库更新: %1 %2").arg(id, err));
}

// ============================================================
// rollbackSelectedModel: 从归档版本里挑一份放回库里
// ============================================================
void frmMain::rollbackSelectedModel()
{
    if (!modelLibTable_) return;
    const int row = modelLibTable_->currentRow();
    if (row < 0 || !modelLibTable_->item(row, 0)) {
        QMessageBox::information(this, "版本回滚", "请先在列表中选中一行模型");
        return;
    }
    const QString id = modelLibTable_->item(row, 0)->text();
    ModelRegistry &reg = ModelRegistry::instance();
    const QStringList versions = reg.archivedVersions(id);
    if (versions.isEmpty()) {
        QMessageBox::information(
            this, "版本回滚",
            QStringLiteral("[%1] 还没有历史版本。\n换过一次权重之后，这里就能选回到上一版。").arg(id));
        return;
    }
    const QString current = reg.meta(id).version;

    bool ok = false;
    // 列表第一项是"当前版本"占位说明，选它等于什么都不做（QInputDialog 没有
    // "只列历史"的选项，把当前版列在最前面反而最容易看明白自己在哪一版）
    QStringList items;
    items << QStringLiteral("（当前版本：%1，不改动）").arg(current.isEmpty() ? QStringLiteral("—") : current);
    items << versions;
    const QString picked = QInputDialog::getItem(
        this, QStringLiteral("版本回滚 [%1]").arg(id),
        QStringLiteral("归档在 data/model_versions/%1/ 的历史版本（新→旧）:").arg(id),
        items, 1, false, &ok);
    if (!ok || picked.isEmpty() || picked == items.first())
        return;
    const QString version = picked;

    if (QtHelper::askConfirm(
            this, QStringLiteral("版本回滚"),
            QStringLiteral("确认把 [%1] 回滚到版本 %2？\n"
                           "现役那份（版本 %3）也会先归档，滚错了还能再滚回来。")
                .arg(id, version, current.isEmpty() ? QStringLiteral("—") : current))
        != QMessageBox::Ok) {
        return;
    }

    QString err;
    if (!reg.rollbackModel(id, version, err)) {
        QMessageBox::warning(this, "回滚失败", err);
        log("model", QStringLiteral("模型回滚失败: %1 %2 %3").arg(id, version, err));
        return;
    }
    const QList<int> channels = ConfigManager::instance().modelParamUsers(id);
    for (int ch : channels)
        applyChannelConfigNow(ch);
    refreshModelLibraryTable();
    refreshModelParamsTable();
    reloadEquipmentService();
    QMessageBox::information(this, "版本回滚",
                             QStringLiteral("已把 [%1] 回滚到 %2，引用通道 %3 路已重开")
                                 .arg(id, version)
                                 .arg(channels.size()));
    log("model", QStringLiteral("模型回滚: %1 -> %2").arg(id, version));
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
// 「把某个地址开到某一路」不再从这里走：这一路的源由 video.channels[i].source_id
// 决定，改引用的唯一出口是 applyChannelSourceChoice() → applyChannelConfigNow()，
// 免得留下第二条"绕过配置直接开流"的路径（它开出来的流和 config 里写的不一定一致）。

// ==========================================
// 4路视频浏览按钮
// ==========================================

// 给"浏览添加"生成一个不撞车的流 id：local-chN，被占用就递增后缀。
// id 是注册表主键，撞车等于把另一路正在用的流改掉，所以宁可一路试到 99 再退到时间戳。
// NOTE: "是否被占用"判的是 videoSource(id).url 为空 —— 注册表里若真存在一条
// **同 id 但地址为空**的条目（只能由手改 JSON 造出来），这里会当成没占用并整条覆盖。
// StreamEditorDialog::onAccept() 会当场拦下空地址，所以正常操作造不出这种条目；
// 若以后放开"允许暂存空地址"，这个判据必须换成"按 id 查存在性"。
static QString freeStreamIdFor(ConfigManager &c, int channel)
{
    const QString base = QStringLiteral("local-ch%1").arg(channel + 1);
    for (int k = 1; k <= 99; ++k) {
        const QString id = (k == 1) ? base : QStringLiteral("%1-%2").arg(base).arg(k);
        if (c.videoSource(id).url.isEmpty())
            return id;
    }
    return base + QStringLiteral("-%1").arg(QDateTime::currentSecsSinceEpoch());
}

/**
 * @brief 浏览某一路的视频文件，把它登记成一条流并让本路引用
 * @param channel 通道号（0-based，与 config 的 video.channels 下标同口径）
 *
 * 选中文件后：建 video.sources[] 条目 → 本路 source_id 指过去 → 落盘 → 立即重开这一路。
 * 不再写内联 source：内联那条路只装得下 url 一个参数，类型/循环/硬解/超时/重连
 * 全要落回默认值，注册表就白建了。
 */
void frmMain::browseChannelSource(int channel)
{
    ConfigManager &c = ConfigManager::instance();
    const QString curUrl = c.resolveChannelSource(channel).url;
    const QString startDir = curUrl.isEmpty() ? QDir::homePath() : QFileInfo(curUrl).path();

    QFileDialog dialog(this, QString("选择通道%1视频文件").arg(channel + 1));
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(startDir);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty())
        return;
    const QString file = dialog.selectedFiles().first();

    VideoSourceCfg s;
    s.id = freeStreamIdFor(c, channel);
    s.type = QStringLiteral("file");
    s.url = file;
    s.note = QStringLiteral("浏览添加");
    s.hardwareDecode = true;
    // loop/timeoutMs/reconnectMs 一律留默认（关 / 0 / 0）：与"内联打开一个文件"的旧行为逐字一致。
    // 要循环播放或断流重连，去「视频流管理…」里改那条流——参数现在有据可依了。
    if (!c.setVideoSource(s)) {
        log("system", QStringLiteral("通道%1 建流失败：id 或地址为空").arg(channel + 1));
        return;
    }
    c.setChannelSourceId(channel, s.id);
    c.save();
    log("system", QStringLiteral("通道%1 已登记视频流 %2：%3")
                    .arg(channel + 1).arg(s.id, file));
    refreshChannelSourceCombos();
    applyChannelConfigNow(channel);
}

// 四颗 .ui 里的浏览按钮（on_<对象名>_clicked 自动连接）。
// 必须是四个独立槽而不是一个带参函数：自动连接按对象名匹配，没有"带控件参数的槽"这条路。
// 1 基对象名 → 0 基通道号的换算只在这里做一次，往下传的都是 0-based。
// 这四颗钮仍在设置页上（initDebugPage 只摘掉了同排的四颗手填地址框）。
void frmMain::on_btnBrowseCh1_clicked() { browseChannelSource(0); }
void frmMain::on_btnBrowseCh2_clicked() { browseChannelSource(1); }
void frmMain::on_btnBrowseCh3_clicked() { browseChannelSource(2); }
void frmMain::on_btnBrowseCh4_clicked() { browseChannelSource(3); }

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
 * @brief 追加日志到文本框（简单文本，无颜色、无时间戳/类别前缀）
 * @param msg 日志消息
 *
 * 与 log()/logWithColor() 的区别不只是"没颜色"：这里走 QTextBrowser::append()
 * 自己的插入路径，**不受 logWithColor() 那套"只在贴底时才跟随"的条件控制**。
 * 所以它只适合偶发的用户级提示（当前唯一调用点是登录失败），
 * 接到报警/围栏这类每秒级路径上就会把视图每次拽回底部——"日志划不动"的老事故。
 * 要带颜色和类别就用 log()。
 */
void frmMain::appendLog(const QString &msg)
{
    ui->textBrowserLog->append(msg);
}
