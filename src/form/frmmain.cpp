#pragma execution_character_set("utf-8")

#include "frmmain.h"
#include "ui_frmmain.h"
#include "core_helper/iconhelper.h"
#include "core_helper/qthelper.h"
#include "SharedTypes.hpp"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>

frmMain::frmMain(QWidget *parent) : QWidget(parent), ui(new Ui::frmMain)
{
    ui->setupUi(this);
    this->initForm();
    this->initStyle();
    this->initLeftMain();
    this->initLeftConfig();
    this->on_btnMenu_Max_clicked();
}

frmMain::~frmMain()
{
    delete ui;
}

bool frmMain::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->widgetTitle) {
        if (event->type() == QEvent::MouseButtonDblClick) {
            on_btnMenu_Max_clicked();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void frmMain::getQssColor(const QString &qss, const QString &flag, QString &color)
{
    int index = qss.indexOf(flag);
    if (index >= 0) {
        color = qss.mid(index + flag.length(), 7);
    }
}

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

QString frmMain::currentTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
}

void frmMain::log(const QString &category, const QString &message)
{
    QColor color;
    QString cat_lower = category.toLower();

    if (cat_lower == "alarm" || cat_lower == "error") {
        color = QColor(220, 50, 50);    // 红色
    } else if (cat_lower == "warning" || cat_lower == "warn") {
        color = QColor(220, 180, 0);    // 黄色
    } else if (cat_lower == "system") {
        color = QColor(50, 120, 220);   // 蓝色
    } else if (cat_lower == "info") {
        color = QColor(50, 180, 50);    // 绿色
    } else {
        color = QColor(180, 180, 180);  // 灰色（默认）
    }

    logWithColor(category, message, color);
}

void frmMain::logWithColor(const QString &category, const QString &message, const QColor &color)
{
    QTextCursor cursor = ui->textBrowserLog->textCursor();
    cursor.movePosition(QTextCursor::End);

    // 添加时间戳（灰色）
    QTextCharFormat tsFormat;
    tsFormat.setForeground(QColor(128, 128, 128));
    cursor.insertText(QString("[%1]").arg(currentTimestamp()), tsFormat);

    // 添加类别（带颜色，加粗）
    QTextCharFormat catFormat;
    catFormat.setForeground(color);
    catFormat.setFontWeight(QFont::Bold);
    cursor.insertText(QString("[%1] ").arg(category), catFormat);

    // 添加消息（带颜色）
    QTextCharFormat msgFormat;
    msgFormat.setForeground(color);
    cursor.insertText(message + "\n", msgFormat);

    // 滚动到底部
    ui->textBrowserLog->setTextCursor(cursor);
    ui->textBrowserLog->ensureCursorVisible();
}

// ==========================================
// UI 初始化
// ==========================================

void frmMain::initForm()
{
    //设置无边框
    QtHelper::setFramelessForm(this);
    //设置图标
    IconHelper::setIcon(ui->labIco, 0xf073, 30);
    IconHelper::setIcon(ui->btnMenu_Min, 0xf068);
    IconHelper::setIcon(ui->btnMenu_Max, 0xf067);
    IconHelper::setIcon(ui->btnMenu_Close, 0xf00d);

    ui->widgetTitle->setProperty("form", "title");
    //关联事件过滤器用于双击放大
    ui->widgetTitle->installEventFilter(this);
    ui->widgetTop->setProperty("nav", "top");

    QFont font;
    font.setPixelSize(15);
    ui->labTitle->setFont(font);
    ui->labTitle->setText("施工行为监测与分析");
    this->setWindowTitle(ui->labTitle->text());

    ui->stackedWidget->setStyleSheet("QLabel{font-size:50px;}");

    QSize icoSize(20, 20);
    int icoWidth = 50;

    //设置顶部导航按钮
    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        btn->setIconSize(icoSize);
        btn->setMinimumWidth(icoWidth);
        btn->setCheckable(true);
        connect(btn, SIGNAL(clicked()), this, SLOT(buttonClick()));
    }

    ui->btnMain->click();

    ui->widgetLeftMain->setProperty("flag", "left");
    ui->widgetLeftConfig->setProperty("flag", "left");
    ui->page1->setStyleSheet(QString("QWidget[flag=\"left\"] QAbstractButton{min-height:%1px;max-height:%1px;}").arg(60));
    ui->page2->setStyleSheet(QString("QWidget[flag=\"left\"] QAbstractButton{min-height:%1px;max-height:%1px;}").arg(25));

    // 初始化调试帮助页面
    initDebugPage();
}

void frmMain::initStyle()
{
    //加载样式表
    QString qss = QtHelper::getStyle(":/qss/blacksoft.css");
    if (!qss.isEmpty()) {
        QString paletteColor = qss.mid(20, 7);
        qApp->setPalette(QPalette(QColor(paletteColor)));
        qApp->setStyleSheet(qss);
    }

    //先从样式表中取出对应的颜色
    QString textColor, panelColor, borderColor, normalColorStart, normalColorEnd, darkColorStart, darkColorEnd, highColor;
    getQssColor(qss, textColor, panelColor, borderColor, normalColorStart, normalColorEnd, darkColorStart, darkColorEnd, highColor);

    //将对应颜色设置到控件
    this->borderColor = highColor;
    this->normalBgColor = normalColorStart;
    this->darkBgColor = panelColor;
    this->normalTextColor = textColor;
    this->darkTextColor = normalTextColor;
}

void frmMain::buttonClick()
{
    QAbstractButton *b = (QAbstractButton *)sender();
    QString name = b->text();

    QList<QAbstractButton *> tbtns = ui->widgetTop->findChildren<QAbstractButton *>();
    foreach (QAbstractButton *btn, tbtns) {
        btn->setChecked(btn == b);
    }

    if (name == "视频监控") {
        ui->stackedWidget->setCurrentIndex(0);
    } else if (name == "系统设置") {
        ui->stackedWidget->setCurrentIndex(1);
    } else if (name == "报警查询") {
        ui->stackedWidget->setCurrentIndex(2);
    } else if (name == "使用帮助") {
        ui->stackedWidget->setCurrentIndex(3);
    } else if (name == "用户退出") {
        systemExit();
    }
}

void frmMain::initLeftMain()
{
    iconsMain << 0xf030 << 0xf03e << 0xf247;

    for (int i = 0; i < btnsMain.count(); ++i) {
        QToolButton *btn = (QToolButton *)btnsMain.at(i);
        btn->setCheckable(true);
        btn->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        connect(btn, SIGNAL(clicked(bool)), this, SLOT(leftMainClick()));
    }

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

    IconHelper::StyleColor styleColor;
    styleColor.position = "left";
    styleColor.iconSize = 16;
    styleColor.iconWidth = 20;
    styleColor.iconHeight = 20;
    styleColor.borderWidth = 3;
    styleColor.borderColor = borderColor;
    styleColor.setColor(normalBgColor, normalTextColor, darkBgColor, darkTextColor);
    IconHelper::setStyle(ui->widgetLeftConfig, btnsConfig, iconsConfig, styleColor);
    ui->tbtnConfig1->click();
}

void frmMain::leftMainClick()
{
    QAbstractButton *b = (QAbstractButton *)sender();
    QString name = b->text();
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

void frmMain::on_btnMenu_Min_clicked()
{
    showMinimized();
}

void frmMain::on_btnMenu_Max_clicked()
{
    static bool max = false;
    static QRect location = this->geometry();

    if (max) {
        this->setGeometry(location);
    } else {
        location = this->geometry();
        this->setGeometry(QtHelper::getScreenRect());
    }

    this->setProperty("canMove", max);
    max = !max;
}

void frmMain::on_btnMenu_Close_clicked()
{
    systemExit();
}

void frmMain::systemExit()
{
    if (!QMessageBox::information(this, tr("退出"), tr("确认要退出系统吗?"), tr("确定"), tr("取消")))
    {
        close();
    }
}

void frmMain::on_pageRoll_customContextMenuRequested(const QPoint &pos)
{
}

// ==========================================
// 调试页面初始化
// ==========================================

void frmMain::initDebugPage()
{
    // 系统信息
    ui->labPlatformVal->setText("RK3588");
    ui->labNpuCoresVal->setText(QString::number(NPU_CORE_NUM));
    ui->labVideoInVal->setText("RTSP / V4L2");
    ui->labMaxChannelsVal->setText("4");

    // 模型配置 - lineEdit already set from UI
    ui->labInputSizeVal->setText("640 x 640");
    ui->labModelTypeVal->setText("YOLO11 (RKNN)");

    // 检测配置 - spinbox already set from UI
    ui->labClassNumVal->setText("80 (COCO)");
    ui->labThreadsVal->setText("3");

    // 初始日志（带颜色）
    log("system", "系统启动");
    log("system", "平台: RK3588, NPU核心数: " + QString::number(NPU_CORE_NUM));
    log("info", "模型加载: " + ui->lineEditModelPath->text());
    log("info", "标签加载: " + ui->lineEditLabelPath->text());
    log("system", "视频解码器初始化完成");
    log("system", "推理线程池启动, 线程数: 3");
}

// ==========================================
// 浏览按钮 - 视频支持文件和文件夹
// ==========================================

void frmMain::on_btnBrowseVideo_clicked()
{
    QString currentPath = ui->lineEditVideoPath->text().isEmpty()
        ? QDir::homePath()
        : ui->lineEditVideoPath->text();

    // 同时支持选择文件和文件夹
    QFileDialog dialog(this, "选择视频文件或文件夹");
    dialog.setFileMode(QFileDialog::ExistingFiles);
    dialog.setOption(QFileDialog::DontUseNativeDialog, false);
    dialog.setDirectory(currentPath);
    dialog.setNameFilter("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)");

    if (dialog.exec() == QDialog::Accepted) {
        QStringList files = dialog.selectedFiles();
        if (!files.isEmpty()) {
            // 如果选了多个文件，用分号分隔；单个直接显示路径
            QString path;
            if (files.size() == 1) {
                QFileInfo fi(files.first());
                if (fi.isDir()) {
                    path = files.first();
                } else {
                    path = files.first();
                }
            } else {
                path = files.join(";");
            }
            ui->lineEditVideoPath->setText(path);
            log("system", QString("视频源已更新: %1").arg(path));
        }
    }
}

void frmMain::on_btnBrowseModel_clicked()
{
    QString currentPath = ui->lineEditModelPath->text().isEmpty()
        ? QDir::currentPath()
        : QFileInfo(ui->lineEditModelPath->text()).path();

    QString file = QFileDialog::getOpenFileName(this, "选择模型文件", currentPath,
        "RKNN模型 (*.rknn);;ONNX模型 (*.onnx);;所有文件 (*)");
    if (!file.isEmpty()) {
        ui->lineEditModelPath->setText(file);
        QFileInfo fi(file);
        log("model", QString("已选择模型: %1 (%2 MB)")
            .arg(fi.fileName())
            .arg(fi.size() / (1024.0 * 1024.0), 0, 'f', 2));
    }
}

void frmMain::on_btnBrowseLabel_clicked()
{
    QString currentPath = ui->lineEditLabelPath->text().isEmpty()
        ? QDir::currentPath()
        : QFileInfo(ui->lineEditLabelPath->text()).path();

    QString file = QFileDialog::getOpenFileName(this, "选择标签文件", currentPath,
        "文本文件 (*.txt);;所有文件 (*)");
    if (!file.isEmpty()) {
        ui->lineEditLabelPath->setText(file);
        log("info", QString("已选择标签文件: %1").arg(QFileInfo(file).fileName()));
    }
}

// ==========================================
// 阈值变化日志
// ==========================================

void frmMain::on_spinBoxConfThresh_valueChanged(double value)
{
    log("system", QString("置信度阈值已更新: %1").arg(value, 0, 'f', 2));
}

void frmMain::on_spinBoxNmsThresh_valueChanged(double value)
{
    log("system", QString("NMS阈值已更新: %1").arg(value, 0, 'f', 2));
}

// ==========================================
// 公共日志接口
// ==========================================

void frmMain::appendLog(const QString &msg)
{
    ui->textBrowserLog->append(msg);
}
