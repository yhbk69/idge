#include "alarm_list_widget.h"
#include "alarm_manager.h"
#include "alarm_detail_dialog.h"
#include "fence_manager.h"
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
#include <QPushButton>

/* 
====================================================
作用：告警列表构造函数
说明：初始化界面，连接信号槽，设置防抖机制
====================================================
*/
AlarmListWidget::AlarmListWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();  // 初始化UI组件
    
    /* 
    ====================================================
    防抖刷新机制
    说明：避免频繁刷新导致界面卡顿
    原理：300ms内的多条告警合并为一次刷新操作
    ====================================================
    */
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);  // 单次触发
    refreshTimer_->setInterval(300);     // 300ms防抖间隔
    connect(refreshTimer_, &QTimer::timeout, this, &AlarmListWidget::onDebouncedRefresh);

    /* 
    ====================================================
    信号槽连接
    说明：监听告警管理器的事件，及时更新界面
    ====================================================
    */
    // 新告警生成信号 -> 处理函数
    connect(&AlarmManager::instance(), &AlarmManager::alarmGenerated,
            this, [this](const AlarmRecord &) {
                onStatsUpdated();          // 立即更新统计数字
                refreshTimer_->start();    // 表格防抖刷新
            });
    // 统计更新信号 -> 处理函数
    connect(&AlarmManager::instance(), &AlarmManager::statsUpdated,
            this, &AlarmListWidget::onStatsUpdated);
    
    refreshTable();  // 初始刷新
}

/* 
====================================================
作用：防抖刷新槽函数
说明：执行实际的表格刷新操作
====================================================
*/
void AlarmListWidget::onDebouncedRefresh()
{
    refreshTable();
    refreshFenceTable();
}

/* 
====================================================
作用：统计更新槽函数
说明：仅更新顶部统计数字，不重建表格
说明：避免频繁刷新时打断用户筛选操作
====================================================
*/
void AlarmListWidget::onStatsUpdated()
{
    AlarmManager &mgr = AlarmManager::instance();  // 获取告警管理器
    
    // 更新总数显示（包含误报数）
    QVector<AlarmRecord> alarms = mgr.alarms();
    int fpCount = 0;
    for (const auto &a : alarms) {
        if (a.isFalsePositive) fpCount++;
    }
    QString statsText = QString("共 %1 条").arg(alarms.size());
    if (fpCount > 0) statsText += QString(" (误报 %1)").arg(fpCount);
    lblTotal_->setText(statsText);
    
    // 更新未确认数显示
    int unack = mgr.unacknowledgedCount();
    lblUnack_->setText(QString("未确认: %1 条").arg(unack));
    
    // 根据未确认数设置不同颜色
    lblUnack_->setStyleSheet(unack > 0
        ? "color: #ff9800; font-size: 13px;"  // 橙色（有未确认）
        : "color: #4caf50; font-size: 13px;"); // 绿色（全部已确认）
}

/* 
====================================================
作用：页面显示事件处理
说明：页面重新可见时刷新一次数据
说明：因为不可见期间只更新了统计数字，需要完整刷新表格
====================================================
*/
void AlarmListWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refreshTable();      // 完整刷新类别报警表格
    refreshFenceTable(); // 完整刷新围栏报警表格
}

/* 
====================================================
作用：设置用户界面
说明：创建告警列表的所有UI组件和布局
====================================================
*/
void AlarmListWidget::setupUi()
{
    // 创建主垂直布局
    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(20, 20, 20, 20);
    mainLay->setSpacing(12);

    // 创建标题标签
    QLabel *header = new QLabel("报警记录");
    header->setStyleSheet("color: #fff; font-size: 20px; font-weight: bold;");
    mainLay->addWidget(header);

    /* 
    ====================================================
    统计信息区域
    说明：显示告警总数和未确认数
    ====================================================
    */
    QHBoxLayout *statsLay = new QHBoxLayout();
    lblTotal_ = new QLabel("共 0 条");  // 总数标签
    lblTotal_->setStyleSheet("color: #aaa; font-size: 13px;");
    statsLay->addWidget(lblTotal_);

    lblUnack_ = new QLabel("未确认: 0 条");  // 未确认数标签
    lblUnack_->setStyleSheet("color: #ff9800; font-size: 13px;");
    statsLay->addWidget(lblUnack_);

    statsLay->addStretch();  // 弹性空间
    mainLay->addLayout(statsLay);

    /* 
    ====================================================
    工具栏
    ====================================================
    */
    QHBoxLayout *toolLay = new QHBoxLayout();
    toolLay->setSpacing(12);
    toolLay->addStretch();  // 弹性空间

    // 刷新按钮
    QPushButton *btnRefresh = new QPushButton("刷新");
    btnRefresh->setStyleSheet("color: #fff; background: #4a6fa5; border-radius: 4px; padding: 6px 16px;");
    connect(btnRefresh, &QPushButton::clicked, this, &AlarmListWidget::onRefresh);
    toolLay->addWidget(btnRefresh);

    // 全部确认按钮
    QPushButton *btnAck = new QPushButton("全部确认");
    btnAck->setStyleSheet("color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;");
    connect(btnAck, &QPushButton::clicked, this, &AlarmListWidget::onAcknowledge);
    toolLay->addWidget(btnAck);

    // 清空按钮
    QPushButton *btnClear = new QPushButton("清空");
    btnClear->setStyleSheet("color: #fff; background: #f44336; border-radius: 4px; padding: 6px 16px;");
    connect(btnClear, &QPushButton::clicked, this, &AlarmListWidget::onClear);
    toolLay->addWidget(btnClear);

    // 打开截图目录按钮
    QPushButton *btnOpenDir = new QPushButton("打开截图目录");
    btnOpenDir->setStyleSheet("color: #fff; background: #ff9800; border-radius: 4px; padding: 6px 16px;");
    connect(btnOpenDir, &QPushButton::clicked, this, &AlarmListWidget::onOpenDir);
    toolLay->addWidget(btnOpenDir);

    /* 
    ====================================================
    截图开关按钮
    说明：控制是否为新告警自动截图
    ====================================================
    */
    snapBtn_ = new QPushButton();
    snapBtn_->setCheckable(true);  // 可切换状态
    bool snapOn = AlarmManager::instance().screenshotsEnabled();  // 获取当前状态
    snapBtn_->setChecked(snapOn);
    snapBtn_->setText(snapOn ? "截图: 开" : "截图: 关");
    snapBtn_->setStyleSheet(snapOn
        ? "color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;"  // 绿色（开启）
        : "color: #aaa; background: #555; border-radius: 4px; padding: 6px 16px;");  // 灰色（关闭）
    
    // 截图开关切换处理
    connect(snapBtn_, &QPushButton::toggled, this, [this](bool on) {
        AlarmManager::instance().setScreenshotsEnabled(on);  // 保存设置
        snapBtn_->setText(on ? "截图: 开" : "截图: 关");
        snapBtn_->setStyleSheet(on
            ? "color: #fff; background: #4caf50; border-radius: 4px; padding: 6px 16px;"
            : "color: #aaa; background: #555; border-radius: 4px; padding: 6px 16px;");
    });
    toolLay->addWidget(snapBtn_);

    mainLay->addLayout(toolLay);

    /* 
    ====================================================
    Tab 切换（类别报警 / 电子围栏）
    ====================================================
    */
    tabWidget_ = new QTabWidget(this);
    tabWidget_->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #444; background: #1e1e2e; }"
        "QTabBar::tab { background: #2d2d3d; color: #aaa; padding: 8px 20px;"
        "  border: 1px solid #444; border-bottom: none; }"
        "QTabBar::tab:selected { background: #1e1e2e; color: #fff; }"
    );

    /* 
    ====================================================
    告警表格（类别报警 Tab）
    ====================================================
    */
    table_ = new QTableWidget();
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "状态", "操作", "ID"});
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
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &AlarmListWidget::onRowDoubleClicked);
    tabWidget_->addTab(table_, "类别报警");

    /* 
    ====================================================
    围栏报警表格（电子围栏 Tab）
    ====================================================
    */
    fenceTable_ = new QTableWidget();
    fenceTable_->setColumnCount(5);
    fenceTable_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "围栏"});
    fenceTable_->horizontalHeader()->setStretchLastSection(true);
    fenceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    fenceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fenceTable_->setAlternatingRowColors(true);
    fenceTable_->setStyleSheet(
        "QTableWidget { background: #1e1e2e; color: #ccc; gridline-color: #333; }"
        "QTableWidget::item:selected { background: #3d5a80; }"
        "QHeaderView::section { background: #2d2d3d; color: #aaa; padding: 6px; border: 1px solid #333; }"
    );
    fenceTable_->verticalHeader()->setVisible(false);
    tabWidget_->addTab(fenceTable_, "电子围栏");

    mainLay->addWidget(tabWidget_, 1);
}

/* 
====================================================
作用：刷新表格数据
说明：从AlarmManager获取告警数据，应用筛选，填充表格
====================================================
*/
void AlarmListWidget::refreshTable()
{
    AlarmManager &mgr = AlarmManager::instance();

    /* 
    ====================================================
    可见性检查
    说明：如果页面不可见，只更新统计数字，不重建表格
    原因：避免频繁刷新导致界面卡顿
    ====================================================
    */
    if (!isVisible()) {
        onStatsUpdated();
        return;
    }

    // 获取所有告警记录，过滤掉误报
    QVector<AlarmRecord> alarms = mgr.alarms();
    QVector<AlarmRecord> filtered;
    for (const auto &a : alarms) {
        if (a.isFalsePositive) continue;
        if (filterChannel_ >= 0 && a.channel != filterChannel_) continue;
        if (!filterClass_.isEmpty() && a.className != filterClass_) continue;
        filtered.append(a);
    }

    // 更新统计信息（总数包含误报）
    int totalAll = alarms.size();
    int totalFiltered = filtered.size();
    int unack = 0;
    int fpCount = 0;
    for (const auto &a : alarms) {
        if (a.isFalsePositive) fpCount++;
        else if (!a.acknowledged) unack++;
    }
    QString statsText = QString("共 %1 条").arg(totalAll);
    if (fpCount > 0) statsText += QString(" (误报 %1)").arg(fpCount);
    lblTotal_->setText(statsText);
    lblUnack_->setText(QString("未确认: %1 条").arg(unack));
    lblUnack_->setStyleSheet(unack > 0
        ? "color: #ff9800; font-size: 13px;"
        : "color: #4caf50; font-size: 13px;");

    /* 
    ====================================================
    填充表格
    说明：只显示最近500条，避免大量数据导致界面卡顿
    说明：倒序显示，最新告警在前
    ====================================================
    */
    const int kMaxRows = 500;  // 最大显示行数
    int total = filtered.size();
    int shown = qMin(total, kMaxRows);  // 实际显示的行数

    table_->setRowCount(shown);  // 设置行数
    for (int i = 0; i < shown; i++) {
        const AlarmRecord &a = filtered[total - 1 - i];  // 倒序获取

        // 存储原始报警索引（用于后续删除操作）
        int originalIndex = total - 1 - i;

        // 时间列
        QDateTime dt;
        dt.setMSecsSinceEpoch(a.timestamp / 1000000);  // 纳秒转换为毫秒
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        timeItem->setData(Qt::UserRole, a.imgPath);  // 存储截图路径
        timeItem->setData(Qt::UserRole + 1, originalIndex);  // 存储原始索引
        table_->setItem(i, 0, timeItem);
        
        // 通道列
        table_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(a.channel + 1)));
        
        // 类别列
        table_->setItem(i, 2, new QTableWidgetItem(a.className));

        // 置信度列
        QTableWidgetItem *confItem = new QTableWidgetItem(QString("%1%").arg(a.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 右对齐
        table_->setItem(i, 3, confItem);

        // 状态列
        QTableWidgetItem *statusItem;
        if (a.isFalsePositive) {
            statusItem = new QTableWidgetItem("误报");
            statusItem->setForeground(QColor(158, 158, 158));  // 灰色
        } else if (a.acknowledged) {
            statusItem = new QTableWidgetItem("已确认");
            statusItem->setForeground(QColor(76, 175, 80));   // 绿色
        } else {
            statusItem = new QTableWidgetItem("未确认");
            statusItem->setForeground(QColor(255, 152, 0));   // 橙色
        }
        table_->setItem(i, 4, statusItem);

        // 操作列（详情按钮）
        QPushButton *btnDetail = new QPushButton("详情");
        btnDetail->setStyleSheet(
            "QPushButton { color: #fff; background: #4a6fa5; border-radius: 4px; padding: 4px 12px; }"
            "QPushButton:hover { background: #5a8fc5; }"
        );
        connect(btnDetail, &QPushButton::clicked, this, [this, i]() {
            showAlarmDetail(i);
        });
        table_->setCellWidget(i, 5, btnDetail);

        // ID列
        table_->setItem(i, 6, new QTableWidgetItem(QString::number(i)));
    }
}

/* 
====================================================
作用：手动刷新槽函数
说明：响应用户点击刷新按钮
====================================================
*/
void AlarmListWidget::onRefresh()
{
    refreshTable();
}

/* 
====================================================
作用：全部确认槽函数
说明：将所有未确认的告警标记为已确认
====================================================
*/
void AlarmListWidget::onAcknowledge()
{
    AlarmManager::instance().acknowledgeAll();  // 确认所有告警
    refreshTable();  // 刷新表格
}

/* 
====================================================
作用：清空告警槽函数
说明：清空所有告警记录（需要用户确认）
====================================================
*/
void AlarmListWidget::onClear()
{
    // 弹出确认对话框
    if (QMessageBox::question(this, "确认", "确定要清空所有报警记录吗？") == QMessageBox::Yes) {
        AlarmManager::instance().clearAlarms();  // 清空告警
        refreshTable();  // 刷新表格
    }
}

/* 
====================================================
作用：打开截图目录槽函数
说明：用系统文件管理器打开告警截图保存目录
====================================================
*/
void AlarmListWidget::onOpenDir()
{
    // 获取告警截图目录路径
    QDir dir("alarms");
    if (!dir.exists()) dir.mkpath(".");  // 如果目录不存在则创建
    QString path = dir.absolutePath();

    // 尝试打开文件管理器
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::information(this, "截图目录",
                                 QString("无法打开文件管理器，截图目录：\n%1").arg(path));
    }
}

/* 
====================================================
作用：表格行双击槽函数
说明：双击行时打开告警详情Dialog
====================================================
*/
void AlarmListWidget::onRowDoubleClicked(int row, int)
{
    showAlarmDetail(row);
}

void AlarmListWidget::refreshFenceTable()
{
    AlarmManager &mgr = AlarmManager::instance();
    QVector<AlarmRecord> alarms = mgr.alarms();

    QVector<AlarmRecord> fenceAlarms;
    for (const auto &a : alarms) {
        if (a.isFenceAlarm) fenceAlarms.append(a);
    }

    const int kMaxRows = 500;
    int total = fenceAlarms.size();
    int shown = qMin(total, kMaxRows);

    fenceTable_->setRowCount(shown);
    for (int i = 0; i < shown; i++) {
        const AlarmRecord &a = fenceAlarms[total - 1 - i];

        QDateTime dt;
        dt.setMSecsSinceEpoch(a.timestamp / 1000000);
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        timeItem->setData(Qt::UserRole, a.imgPath);
        fenceTable_->setItem(i, 0, timeItem);

        fenceTable_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(a.channel + 1)));
        fenceTable_->setItem(i, 2, new QTableWidgetItem(a.className));

        QTableWidgetItem *confItem = new QTableWidgetItem(QString("%1%").arg(a.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        fenceTable_->setItem(i, 3, confItem);

        fenceTable_->setItem(i, 4, new QTableWidgetItem("围栏"));
    }
}

/* 
====================================================
作用：显示告警详情Dialog
说明：打开详情对话框，支持误报标记
参数：row - 表格行号
====================================================
*/
void AlarmListWidget::showAlarmDetail(int row)
{
    QTableWidgetItem *timeItem = table_->item(row, 0);
    if (!timeItem) return;

    int originalIndex = timeItem->data(Qt::UserRole + 1).toInt();
    QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();
    if (originalIndex < 0 || originalIndex >= alarms.size()) return;

    AlarmRecord alarm = alarms[originalIndex];

    AlarmDetailDialog dlg(alarm, originalIndex, this);
    connect(&dlg, &AlarmDetailDialog::alarmMarkedFalsePositive, this, [this](int idx) {
        AlarmManager::instance().markAsFalsePositive(idx);
        refreshTable();
    });
    connect(&dlg, &AlarmDetailDialog::alarmAcknowledged, this, [this](int idx) {
        AlarmManager::instance().acknowledgeAlarm(idx);
        refreshTable();
    });
    dlg.exec();
}

/* 
====================================================
作用：表格标题行点击槽函数
说明：点击标题时弹出筛选菜单
参数：logicalIndex - 列号
====================================================
*/
void AlarmListWidget::onHeaderSectionClicked(int logicalIndex)
{
    // 只处理通道列(1)和类别列(2)
    if (logicalIndex != 1 && logicalIndex != 2) return;

    QMenu menu(this);
    menu.setStyleSheet(
        "QMenu { background: #2d2d3d; color: #ccc; border: 1px solid #444; }"
        "QMenu::item:selected { background: #3d5a80; }"
    );

    if (logicalIndex == 1) {
        // 通道筛选
        menu.addAction("全部通道", this, [this]() {
            filterChannel_ = -1;
            refreshTable();
        });
        menu.addAction("通道 1", this, [this]() {
            filterChannel_ = 0;
            refreshTable();
        });
        menu.addAction("通道 2", this, [this]() {
            filterChannel_ = 1;
            refreshTable();
        });
        menu.addAction("通道 3", this, [this]() {
            filterChannel_ = 2;
            refreshTable();
        });
        menu.addAction("通道 4", this, [this]() {
            filterChannel_ = 3;
            refreshTable();
        });
    } else if (logicalIndex == 2) {
        // 类别筛选
        QSet<QString> classes;
        QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();
        for (const auto &a : alarms) {
            if (!a.isFalsePositive) classes.insert(a.className);
        }

        menu.addAction("全部类别", this, [this]() {
            filterClass_.clear();
            refreshTable();
        });
        for (const auto &cls : classes) {
            menu.addAction(cls, this, [this, cls]() {
                filterClass_ = cls;
                refreshTable();
            });
        }
    }

    menu.exec(QCursor::pos());
}