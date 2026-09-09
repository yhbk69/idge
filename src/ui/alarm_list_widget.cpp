#include "alarm_list_widget.h"
#include "alarm_manager.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDateTime>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QShowEvent>

AlarmListWidget::AlarmListWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
    // 报警实时刷新（防抖）：新报警先更新统计数字，表格用定时器合并刷新，
    // 避免"报警很频繁时每条都整表重建"把主线程拖卡
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(300);   // 300ms 内的多条报警合并成一次刷新
    connect(refreshTimer_, &QTimer::timeout, this, &AlarmListWidget::onDebouncedRefresh);

    connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
            this, [this](const AlarmRecord &) {
                onStatsUpdated();          // 数字立刻更新
                if (isVisible()) {
                    refreshTimer_->start();  // 表格防抖刷新
                }
            });
    connect(&AlarmManager::instance(), &AlarmManager::statsUpdated,
            this, &AlarmListWidget::onStatsUpdated);
    refreshTable();
}

void AlarmListWidget::onDebouncedRefresh()
{
    refreshTable();
}

void AlarmListWidget::onStatsUpdated()
{
    // 仅刷新顶部统计数字，不重建表格（避免频繁打断筛选操作）
    // 注意用 alarmCount() 只取数量，不要整表拷贝，否则报警多时会拖慢主线程
    AlarmManager &mgr = AlarmManager::instance();
    lblTotal_->setText(QString("共 %1 条").arg(mgr.alarmCount()));
    int unack = mgr.unacknowledgedCount();
    lblUnack_->setText(QString("未确认: %1 条").arg(unack));
    lblUnack_->setStyleSheet(unack > 0
        ? "color: #ff9800; font-size: 13px;"
        : "color: #4caf50; font-size: 13px;");
}

void AlarmListWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 页面重新可见时做一次完整刷新（因为不可见期间只更新了统计数字）
    refreshTable();
}

void AlarmListWidget::setupUi()
{
    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(20, 20, 20, 20);
    mainLay->setSpacing(12);

    // 标题行
    QLabel *header = new QLabel("报警记录");
    header->setStyleSheet("color: #fff; font-size: 20px; font-weight: bold;");
    mainLay->addWidget(header);

    // 统计信息
    QHBoxLayout *statsLay = new QHBoxLayout();
    lblTotal_ = new QLabel("共 0 条");
    lblTotal_->setStyleSheet("color: #aaa; font-size: 13px;");
    statsLay->addWidget(lblTotal_);

    lblUnack_ = new QLabel("未确认: 0 条");
    lblUnack_->setStyleSheet("color: #ff9800; font-size: 13px;");
    statsLay->addWidget(lblUnack_);

    statsLay->addStretch();
    mainLay->addLayout(statsLay);

    // 筛选工具栏
    QHBoxLayout *toolLay = new QHBoxLayout();
    toolLay->setSpacing(12);

    QLabel *chLbl = new QLabel("通道:");
    chLbl->setStyleSheet("color: #aaa; font-size: 12px;");
    toolLay->addWidget(chLbl);

    filterChannel_ = new QComboBox();
    filterChannel_->addItem("全部", -1);
    filterChannel_->addItem("通道 1", 0);
    filterChannel_->addItem("通道 2", 1);
    filterChannel_->addItem("通道 3", 2);
    filterChannel_->addItem("通道 4", 3);
    filterChannel_->setStyleSheet("color: #fff; background: #3d3d4d; border: 1px solid #555; border-radius: 4px; padding: 4px 8px;");
    toolLay->addWidget(filterChannel_);

    QLabel *clsLbl = new QLabel("类别:");
    clsLbl->setStyleSheet("color: #aaa; font-size: 12px;");
    toolLay->addWidget(clsLbl);

    filterClass_ = new QComboBox();
    filterClass_->addItem("全部", "");
    filterClass_->setStyleSheet("color: #fff; background: #3d3d4d; border: 1px solid #555; border-radius: 4px; padding: 4px 8px;");
    toolLay->addWidget(filterClass_);

    toolLay->addStretch();

    QPushButton *btnRefresh = new QPushButton("刷新");
    btnRefresh->setStyleSheet("color: #fff; background: #4a6fa5; border-radius: 4px; padding: 6px 16px;");
    connect(btnRefresh, &QPushButton::clicked, this, &AlarmListWidget::onRefresh);
    toolLay->addWidget(btnRefresh);

    QPushButton *btnAck = new QPushButton("全部确认");
    btnAck->setStyleSheet("color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;");
    connect(btnAck, &QPushButton::clicked, this, &AlarmListWidget::onAcknowledge);
    toolLay->addWidget(btnAck);

    QPushButton *btnClear = new QPushButton("清空");
    btnClear->setStyleSheet("color: #fff; background: #f44336; border-radius: 4px; padding: 6px 16px;");
    connect(btnClear, &QPushButton::clicked, this, &AlarmListWidget::onClear);
    toolLay->addWidget(btnClear);

    // 打开报警截图目录（用系统文件管理器查看截图文件）
    QPushButton *btnOpenDir = new QPushButton("打开截图目录");
    btnOpenDir->setStyleSheet("color: #fff; background: #ff9800; border-radius: 4px; padding: 6px 16px;");
    connect(btnOpenDir, &QPushButton::clicked, this, &AlarmListWidget::onOpenDir);
    toolLay->addWidget(btnOpenDir);

    // 报警截图开关：可勾选，关掉后新报警不再自动截图
    snapBtn_ = new QPushButton();
    snapBtn_->setCheckable(true);
    bool snapOn = AlarmManager::instance().screenshotsEnabled();
    snapBtn_->setChecked(snapOn);
    snapBtn_->setText(snapOn ? "截图: 开" : "截图: 关");
    snapBtn_->setStyleSheet(snapOn
        ? "color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;"
        : "color: #aaa; background: #555; border-radius: 4px; padding: 6px 16px;");
    connect(snapBtn_, &QPushButton::toggled, this, [this](bool on) {
        // 截图开关：状态存到 AlarmManager，解码线程报警时据此决定是否截图
        AlarmManager::instance().setScreenshotsEnabled(on);
        snapBtn_->setText(on ? "截图: 开" : "截图: 关");
        snapBtn_->setStyleSheet(on
            ? "color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;"
            : "color: #aaa; background: #555; border-radius: 4px; padding: 6px 16px;");
    });
    toolLay->addWidget(snapBtn_);

    mainLay->addLayout(toolLay);

    // 报警表格
    table_ = new QTableWidget();
    table_->setColumnCount(6);
    table_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "状态", "ID"});
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet(
        "QTableWidget { background: #1e1e2e; color: #ccc; gridline-color: #333; }"
        "QTableWidget::item:selected { background: #3d5a80; }"
        "QHeaderView::section { background: #2d2d3d; color: #aaa; padding: 6px; border: 1px solid #333; }"
    );
    table_->verticalHeader()->setVisible(false);
    // 双击某一行：直接用系统看图程序打开该报警的截图
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &AlarmListWidget::onRowDoubleClicked);
    mainLay->addWidget(table_, 1);
}

void AlarmListWidget::refreshTable()
{
    AlarmManager &mgr = AlarmManager::instance();

    // 当前页不可见时：只更新顶部统计数字，不重建表格。
    // 原因是报警刷新会整表重建(每次最多1000行)，若报警很频繁，
    // 即使切到别的页面也会在主线程反复重建，把界面拖到"看起来卡死"。
    // 切回本页时 showEvent() 会做一次完整刷新。
    if (!isVisible()) {
        onStatsUpdated();
        return;
    }

    QVector<AlarmRecord> alarms = mgr.alarms();

    // 更新统计
    lblTotal_->setText(QString("共 %1 条").arg(alarms.size()));
    int unack = mgr.unacknowledgedCount();
    lblUnack_->setText(QString("未确认: %1 条").arg(unack));
    lblUnack_->setStyleSheet(unack > 0
        ? "color: #ff9800; font-size: 13px;"
        : "color: #4caf50; font-size: 13px;");

    // 更新筛选下拉框的类别列表
    QSet<QString> classes;
    for (const auto &a : alarms)
        classes.insert(a.className);
    QString currentClass = filterClass_->currentText();
    filterClass_->clear();
    filterClass_->addItem("全部", "");
    for (const auto &cls : classes) {
        filterClass_->addItem(cls, cls);
    }
    int idx = filterClass_->findText(currentClass);
    if (idx >= 0) filterClass_->setCurrentIndex(idx);

    // 筛选
    int filterCh = filterChannel_->currentData().toInt();
    QString filterCls = filterClass_->currentData().toString();

    QVector<AlarmRecord> filtered;
    for (const auto &a : alarms) {
        if (filterCh >= 0 && a.channel != filterCh) continue;
        if (!filterCls.isEmpty() && a.className != filterCls) continue;
        filtered.append(a);
    }

    // 填充表格（倒序，最新在前）
    // 只显示最近 kMaxRows 条，防止报警上万条时整表重建把主线程拖卡
    const int kMaxRows = 500;
    int total = filtered.size();
    int shown = qMin(total, kMaxRows);          // 本次显示的行数

    table_->setRowCount(shown);
    for (int i = 0; i < shown; i++) {
        const AlarmRecord &a = filtered[total - 1 - i];

        QDateTime dt;
        dt.setMSecsSinceEpoch(a.timestamp / 1000000);  // ns -> ms
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        // 把截图路径存在这一格的自定义数据里，双击时取出来打开
        timeItem->setData(Qt::UserRole, a.imgPath);
        table_->setItem(i, 0, timeItem);
        table_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(a.channel + 1)));
        table_->setItem(i, 2, new QTableWidgetItem(a.className));

        QTableWidgetItem *confItem = new QTableWidgetItem(QString("%1%").arg(a.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        table_->setItem(i, 3, confItem);

        QTableWidgetItem *statusItem = new QTableWidgetItem(
            a.acknowledged ? "已确认" : "未确认");
        statusItem->setForeground(a.acknowledged ? QColor(76, 175, 80) : QColor(255, 152, 0));
        table_->setItem(i, 4, statusItem);

        table_->setItem(i, 5, new QTableWidgetItem(QString::number(i)));
    }
}

void AlarmListWidget::onRefresh()
{
    refreshTable();
}

void AlarmListWidget::onAcknowledge()
{
    AlarmManager::instance().acknowledgeAll();
    refreshTable();
}

void AlarmListWidget::onClear()
{
    if (QMessageBox::question(this, "确认", "确定要清空所有报警记录吗？") == QMessageBox::Yes) {
        AlarmManager::instance().clearAlarms();
        refreshTable();
    }
}

void AlarmListWidget::onOpenDir()
{
    // 用系统文件管理器打开报警截图目录（没有截图时也创建目录再打开）
    QDir dir("alarms");
    if (!dir.exists()) dir.mkpath(".");
    QString path = dir.absolutePath();

    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::information(this, "截图目录",
                                 QString("无法打开文件管理器，截图目录：\n%1").arg(path));
    }
}

void AlarmListWidget::onRowDoubleClicked(int row, int)
{
    // 双击行 -> 打开该条报警对应的截图
    QTableWidgetItem *it = table_->item(row, 0);
    if (!it) return;
    QString imgPath = it->data(Qt::UserRole).toString();
    if (imgPath.isEmpty() || !QFile::exists(imgPath)) {
        QMessageBox::information(this, "截图", "该报警没有截图记录");
        return;
    }
    QString abs = QFileInfo(imgPath).absoluteFilePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(abs))) {
        QMessageBox::information(this, "截图",
                                 QString("无法打开图片：\n%1").arg(abs));
    }
}
