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
#include "core_helper/iconhelper.h"
#include "core_helper/qthelper.h"
#include "SharedTypes.hpp"
#include "ConfigManager.h"
#include "frmvideowindow.h"
#include "player_widget.h"
#include "dashboard_widget.h"
#include "alarm_list_widget.h"
#include "alarm_manager.h"
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

// ==========================================
// 构造 / 析构
// ==========================================

frmMain::frmMain(QWidget *parent) : QWidget(parent), ui(new Ui::frmMain)
{
    ui->setupUi(this);
    this->initForm();
    this->initStyle();
    this->initNewPages();
    this->initLeftMain();
    this->initLeftConfig();
    this->on_btnMenu_Max_clicked();
}

frmMain::~frmMain()
{
    delete ui;
}

// ==========================================
// 事件过滤器
// ==========================================

/**
 * @brief 事件过滤器 - 拦截标题栏双击事件，实现最大化/还原切换
 */
bool frmMain::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->widgetTitle) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            on_btnMenu_Max_clicked();
        }
    }
    return QWidget::eventFilter(watched, event);
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

    // 自动滚动到底部
    ui->textBrowserLog->setTextCursor(cursor);
    ui->textBrowserLog->ensureCursorVisible();
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
    IconHelper::setIcon(ui->labIco, 0xf073, 30);       // 日历图标
    IconHelper::setIcon(ui->btnMenu_Min, 0xf068);       // 最小化
    IconHelper::setIcon(ui->btnMenu_Max, 0xf067);       // 最大化
    IconHelper::setIcon(ui->btnMenu_Close, 0xf00d);     // 关闭

    // 3. 标题栏属性
    ui->widgetTitle->setProperty("form", "title");
    ui->widgetTitle->installEventFilter(this);  // 双击最大化
    ui->widgetTop->setProperty("nav", "top");

    // 4. 设置标题文字
    QFont font;
    font.setPixelSize(15);
    ui->labTitle->setFont(font);
    ui->labTitle->setText("施工行为监测与分析");
    this->setWindowTitle(ui->labTitle->text());

    // 5. stackedWidget 全局样式（页面内控件会覆盖）
    ui->stackedWidget->setStyleSheet("QLabel{font-size:50px;}");

    // 6. 配置顶部导航按钮
    QSize icoSize(22, 22);
    int icoWidth = 56;
    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        QToolButton *tbtn = qobject_cast<QToolButton *>(btn);
        btn->setIconSize(icoSize);
        btn->setMinimumWidth(icoWidth);
        btn->setCheckable(true);
        if (tbtn) {
            // 图标+文字（不只是图标）
            tbtn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            tbtn->setMinimumHeight(58);
            QFont bf = tbtn->font();
            bf.setPixelSize(12);
            tbtn->setFont(bf);
        }
        connect(btn, SIGNAL(clicked()), this, SLOT(buttonClick()));
    }

    // 默认选中"视频监控"
    ui->btnMain->click();

    // 7. 左侧导航按钮样式
    ui->widgetLeftMain->setProperty("flag", "left");
    ui->widgetLeftConfig->setProperty("flag", "left");
    ui->page1->setStyleSheet(QString("QWidget[flag=\"left\"] QAbstractButton{min-height:%1px;max-height:%1px;}").arg(60));
    ui->page2->setStyleSheet(QString("QWidget[flag=\"left\"] QAbstractButton{min-height:%1px;max-height:%1px;}").arg(25));

    // 8. 获取视频监控窗口指针（用于后续打开视频）
    videoWindow = ui->lab1;

    // 9. 加载配置文件
    ConfigManager::instance().load("config.json");

    // 10. 从配置加载视频到4个通道（必须在 config 加载之后）
    ConfigManager &cfg = ConfigManager::instance();
    videoWindow->openVideo(0, cfg.videoChannel(1));
    videoWindow->openVideo(1, cfg.videoChannel(2));
    videoWindow->openVideo(2, cfg.videoChannel(3));
    videoWindow->openVideo(3, cfg.videoChannel(4));

    // 11. 初始化调试帮助页（从 config 读取配置填充 UI）
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

    // ===== 顶部导航收敛为 5 项 =====
    ui->btnMain->setText("视频监控");
    ui->btnRoll->setText("数据看板");
    ui->btnEquipCheck->setText("报警数据");
    ui->btnHelp->setText("调试设置");
    ui->btnExit->setText("退出");
    // 隐藏多余的占位按钮（人员点名/设备盘点原义、报警查询、系统设置）
    ui->btnData->hide();
    ui->btnConfig->hide();

    // 向 stackedWidget 添加数据看板和报警记录页面
    dashboardWidget_ = new DashboardWidget();
    alarmListWidget_ = new AlarmListWidget();

    // 插入到 page1(视频监控) 之后：index 1=数据看板, 2=报警数据
    ui->stackedWidget->insertWidget(1, dashboardWidget_);
    ui->stackedWidget->insertWidget(2, alarmListWidget_);

    // 设置背景
    dashboardWidget_->setStyleSheet("background: #1e1e2e;");
    alarmListWidget_->setStyleSheet("background: #1e1e2e;");

    // ===== 悬浮报警提示 toast =====
    alarmToast_ = new QLabel(this);
    alarmToast_->setStyleSheet(
        "QLabel { background: rgba(220,50,50,0.92); color: white;"
        " border-radius: 6px; padding: 10px 16px; font-size: 14px; font-weight: bold; }");
    alarmToast_->setVisible(false);
    alarmToast_->setWordWrap(true);
    alarmToast_->adjustSize();
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, alarmToast_, &QLabel::hide);

    // ===== 未确认报警角标 =====
    alarmBadge_ = new QLabel(ui->btnEquipCheck);
    alarmBadge_->setStyleSheet(
        "QLabel { background: #f44336; color: white; border-radius: 8px;"
        " font-size: 10px; font-weight: bold; min-width: 15px; max-width: 40px;"
        " padding: 1px 3px; }");
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

    // 设置类名到 AlarmManager（从模型加载）
    QStringList classNames;
    QFile labelFile(ConfigManager::instance().labelPath());
    if (labelFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!labelFile.atEnd()) {
            QString line = QString::fromUtf8(labelFile.readLine()).trimmed();
            if (!line.isEmpty()) classNames.append(line);
        }
        labelFile.close();
    }
    AlarmManager::instance().setClassNames(classNames);

    updateAlarmBadge();
}

void frmMain::showAlarmToast(const AlarmRecord &alarm)
{
    if (!alarmToast_) return;

    QDateTime dt = QDateTime::fromMSecsSinceEpoch(alarm.timestamp / 1000000);
    alarmToast_->setText(QString("⚠ 通道%1 检测到 %2 (%3%%)  %4")
        .arg(alarm.channel + 1)
        .arg(alarm.className)
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

void frmMain::updateAlarmBadge()
{
    if (!alarmBadge_) return;
    int unack = AlarmManager::instance().unacknowledgedCount();
    if (unack > 0) {
        alarmBadge_->setText(QString::number(unack > 99 ? 99 : unack));
        alarmBadge_->adjustSize();
        // 定位到按钮右上角
        int bw = ui->btnEquipCheck->width();
        alarmBadge_->move(bw - alarmBadge_->width() - 2, 2);
        alarmBadge_->show();
        alarmBadge_->raise();
    } else {
        alarmBadge_->hide();
    }
}

// ==========================================
// 导航按钮点击处理
// ==========================================

/**
 * @brief 顶部导航按钮点击处理
 * 根据按钮文字切换 stackedWidget 页面（用 setCurrentWidget 按控件指针切换，避免索引漂移）：
 *   - "视频监控" -> page1（视频监控页）
 *   - "数据看板" -> dashboardWidget_（数据看板）
 *   - "报警数据" -> alarmListWidget_（报警数据）
 *   - "调试设置" -> page4（调试帮助页）
 *   - "退出"     -> 退出确认
 */
void frmMain::buttonClick()
{
    QAbstractButton *b = (QAbstractButton *)sender();
    QString name = b->text();

    // 更新按钮选中状态
    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        btn->setChecked(btn == b);
    }

    // 切换页面
    if (name == "视频监控") {
        ui->stackedWidget->setCurrentWidget(ui->page1);
    } else if (name == "数据看板") {
        ui->stackedWidget->setCurrentWidget(dashboardWidget_);
    } else if (name == "报警数据") {
        ui->stackedWidget->setCurrentWidget(alarmListWidget_);
    } else if (name == "调试设置") {
        ui->stackedWidget->setCurrentWidget(ui->page4);
    } else if (name == "退出") {
        systemExit();
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

/**
 * @brief 初始化系统设置页左侧导航
 * 6个配置子菜单：基本设置、转发设置、用户设置、防区设置、设备设置、其他设置
 */
void frmMain::initLeftConfig()
{
    iconsConfig << 0xf031 << 0xf036 << 0xf249 << 0xf055 << 0xf05a << 0xf249;
    btnsConfig << ui->tbtnConfig1 << ui->tbtnConfig2 << ui->tbtnConfig3 << ui->tbtnConfig4 << ui->tbtnConfig5 << ui->tbtnConfig6;

    for (int i = 0; i < btnsConfig.count(); ++i) {
        QToolButton *btn = (QToolButton *)btnsConfig.at(i);
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        connect(btn, SIGNAL(clicked(bool)), this, SLOT(leftConfigClick()));
    }

    // 配置导航栏样式颜色
    IconHelper::StyleColor styleColor;
    styleColor.position = "left";
    styleColor.iconSize = 16;
    styleColor.iconWidth = 20;
    styleColor.iconHeight = 20;
    styleColor.borderWidth = 3;
    styleColor.borderColor = borderColor;
    styleColor.setColor(normalBgColor, normalTextColor, darkBgColor, darkTextColor);
    IconHelper::setStyle(ui->widgetLeftConfig, btnsConfig, iconsConfig, styleColor);

    // 默认选中"基本设置"
    ui->tbtnConfig1->click();
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

void frmMain::leftConfigClick()
{
    QToolButton *b = (QToolButton *)sender();
    QString name = b->text();
    for (int i = 0; i < btnsConfig.count(); ++i) {
        QAbstractButton *btn = btnsConfig.at(i);
        btn->setChecked(btn == b);
    }
    ui->lab2->setText(name);
}

// ==========================================
// 窗口控制按钮
// ==========================================

void frmMain::on_btnMenu_Min_clicked()
{
    showMinimized();
}

/**
 * @brief 最大化/还原窗口切换
 * 使用 static 变量记录还原前的位置
 */
void frmMain::on_btnMenu_Max_clicked()
{
    static bool max = false;
    static QRect location = this->geometry();

    if (max) {
        this->setGeometry(location);           // 还原
    } else {
        location = this->geometry();           // 记录当前位置
        this->setGeometry(QtHelper::getScreenRect());  // 全屏
    }

    this->setProperty("canMove", max);
    max = !max;
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
    QMessageBox box(QMessageBox::Question, tr("退出系统"), tr("确认要退出系统吗?"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QAbstractButton *okBtn = box.button(QMessageBox::Ok);
    if (okBtn) okBtn->setText(tr("确定"));
    QAbstractButton *cancelBtn = box.button(QMessageBox::Cancel);
    if (cancelBtn) cancelBtn->setText(tr("取消"));
    box.adjustSize();

    // 居中显示
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();

    if (box.exec() == QMessageBox::Ok) {
        close();
    }
}

void frmMain::closeEvent(QCloseEvent *event)
{
    // 停止所有 4 路解码器和推理任务
    if (videoWindow) {
        for (int ch = 0; ch < 4; ch++) {
            PlayerWidget *pw = videoWindow->playerWidget(ch);
            if (pw) {
                pw->stopDecoder();
            }
        }
    }
    event->accept();
}

void frmMain::on_pageRoll_customContextMenuRequested(const QPoint &pos)
{
}

// ==========================================
// 调试帮助页初始化
// ==========================================

/**
 * @brief 初始化调试帮助页
 *
 * 从 config.json 读取所有配置并填充到 UI 控件：
 *   - 系统信息：平台、NPU核心数、视频接入方式、最大通道数
 *   - 视频通道：4路视频源路径（lineEditCh1~4）
 *   - 模型配置：模型路径、标签路径、输入尺寸、模型类型
 *   - 检测配置：置信度阈值、NMS阈值、类别数、线程数
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

    // ========== 视频通道（从 config.json 读取） ==========
    ui->lineEditCh1->setText(cfg.videoChannel(1));
    ui->lineEditCh2->setText(cfg.videoChannel(2));
    ui->lineEditCh3->setText(cfg.videoChannel(3));
    ui->lineEditCh4->setText(cfg.videoChannel(4));

    // ========== 模型配置（从 config.json 读取） ==========
    ui->lineEditModelPath->setText(cfg.modelPath());
    ui->lineEditLabelPath->setText(cfg.labelPath());
    ui->labInputSizeVal->setText(cfg.inputSize());
    ui->labModelTypeVal->setText(cfg.modelType());

    // ========== 检测配置（从 config.json 读取） ==========
    ui->spinBoxConfThresh->setValue(cfg.confThreshold());
    ui->spinBoxNmsThresh->setValue(cfg.nmsThreshold());
    ui->labClassNumVal->setText(QString::number(cfg.classNum()));
    ui->labThreadsVal->setText(QString::number(cfg.threads()));

    // ========== 初始日志（带颜色） ==========
    log("system", "系统启动");
    log("system", "平台: RK3588, NPU核心数: " + QString::number(NPU_CORE_NUM));
    log("info", "模型加载: " + cfg.modelPath());
    log("info", "标签加载: " + cfg.labelPath());
    log("system", "视频解码器初始化完成");
    log("system", "推理线程池启动, 线程数: " + QString::number(cfg.threads()));
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

    PlayerWidget *player = videoWindow->playerWidget(channel);
    if (player && !path.isEmpty()) {
        player->open(path.toStdString());
        log("system", QString("通道%1 视频源已加载: %2").arg(channel + 1).arg(path));
    }
}

// ==========================================
// 4路视频浏览按钮
// ==========================================

/**
 * @brief 通道1 视频浏览
 * 打开文件选择对话框，选择视频文件后：
 *   1. 更新 lineEditCh1 显示路径
 *   2. 保存到 config.json
 *   3. 加载视频到 videoWindow1
 */
void frmMain::on_btnBrowseCh1_clicked()
{
    QString currentPath = ui->lineEditCh1->text().isEmpty()
        ? QDir::homePath()
        : QFileInfo(ui->lineEditCh1->text()).path();

    QFileDialog dialog(this, "选择通道1视频文件");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        QString file = dialog.selectedFiles().first();
        ui->lineEditCh1->setText(file);
        ConfigManager::instance().setVideoChannel(1, file);
        ConfigManager::instance().save();
        openVideoToChannel(0, file);
    }
}

void frmMain::on_btnBrowseCh2_clicked()
{
    QString currentPath = ui->lineEditCh2->text().isEmpty()
        ? QDir::homePath()
        : QFileInfo(ui->lineEditCh2->text()).path();

    QFileDialog dialog(this, "选择通道2视频文件");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        QString file = dialog.selectedFiles().first();
        ui->lineEditCh2->setText(file);
        ConfigManager::instance().setVideoChannel(2, file);
        ConfigManager::instance().save();
        openVideoToChannel(1, file);
    }
}

void frmMain::on_btnBrowseCh3_clicked()
{
    QString currentPath = ui->lineEditCh3->text().isEmpty()
        ? QDir::homePath()
        : QFileInfo(ui->lineEditCh3->text()).path();

    QFileDialog dialog(this, "选择通道3视频文件");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        QString file = dialog.selectedFiles().first();
        ui->lineEditCh3->setText(file);
        ConfigManager::instance().setVideoChannel(3, file);
        ConfigManager::instance().save();
        openVideoToChannel(2, file);
    }
}

void frmMain::on_btnBrowseCh4_clicked()
{
    QString currentPath = ui->lineEditCh4->text().isEmpty()
        ? QDir::homePath()
        : QFileInfo(ui->lineEditCh4->text()).path();

    QFileDialog dialog(this, "选择通道4视频文件");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        QString file = dialog.selectedFiles().first();
        ui->lineEditCh4->setText(file);
        ConfigManager::instance().setVideoChannel(4, file);
        ConfigManager::instance().save();
        openVideoToChannel(3, file);
    }
}

// ==========================================
// 模型和标签浏览按钮
// ==========================================

/**
 * @brief 模型文件浏览
 * 支持格式: .rknn (RKNN模型), .onnx (ONNX模型)
 * 选择后保存到 config.json 并输出日志（含文件大小）
 */
void frmMain::on_btnBrowseModel_clicked()
{
    QString currentPath = ui->lineEditModelPath->text().isEmpty()
        ? QDir::currentPath()
        : QFileInfo(ui->lineEditModelPath->text()).path();

    QString file = QFileDialog::getOpenFileName(this, "选择模型文件", currentPath,
        "RKNN模型 (*.rknn);;ONNX模型 (*.onnx);;所有文件 (*)");
    if (!file.isEmpty()) {
        ui->lineEditModelPath->setText(file);
        ConfigManager::instance().setModelPath(file);
        ConfigManager::instance().save();
        QFileInfo fi(file);
        log("model", QString("已选择模型: %1 (%2 MB)")
            .arg(fi.fileName())
            .arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 2));
    }
}

/**
 * @brief 标签文件浏览
 * 支持格式: .txt
 * 选择后保存到 config.json 并输出日志
 */
void frmMain::on_btnBrowseLabel_clicked()
{
    QString currentPath = ui->lineEditLabelPath->text().isEmpty()
        ? QDir::currentPath()
        : QFileInfo(ui->lineEditLabelPath->text()).path();

    QString file = QFileDialog::getOpenFileName(this, "选择标签文件", currentPath,
        "文本文件 (*.txt);;所有文件 (*)");
    if (!file.isEmpty()) {
        ui->lineEditLabelPath->setText(file);
        ConfigManager::instance().setLabelPath(file);
        ConfigManager::instance().save();
        log("info", QString("已选择标签文件: %1").arg(QFileInfo(file).fileName()));
    }
}

// ==========================================
// 阈值变化 -> 保存配置 + 日志
// ==========================================

/**
 * @brief 置信度阈值变化槽
 * 当用户在 spinBox 中修改置信度时：
 *   1. 保存到 config.json
 *   2. 输出日志
 */
void frmMain::on_spinBoxConfThresh_valueChanged(double value)
{
    ConfigManager::instance().setConfThreshold(value);
    ConfigManager::instance().save();
    log("system", QString("置信度阈值已更新: %1").arg(value, 0, 'f', 2));
}

/**
 * @brief NMS 阈值变化槽
 * 当用户在 spinBox 中修改 NMS 阈值时：
 *   1. 保存到 config.json
 *   2. 输出日志
 */
void frmMain::on_spinBoxNmsThresh_valueChanged(double value)
{
    ConfigManager::instance().setNmsThreshold(value);
    ConfigManager::instance().save();
    log("system", QString("NMS阈值已更新: %1").arg(value, 0, 'f', 2));
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
