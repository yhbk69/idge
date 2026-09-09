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
                if (isVisible()) {
                    refreshTimer_->start();  // 表格防抖刷新
                }
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
    
    // 更新总数显示
    lblTotal_->setText(QString("共 %1 条").arg(mgr.alarmCount()));
    
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
    refreshTable();  // 完整刷新表格
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
    筛选工具栏
    说明：提供通道和类别筛选功能
    ====================================================
    */
    QHBoxLayout *toolLay = new QHBoxLayout();
    toolLay->setSpacing(12);

    // 通道筛选标签和下拉框
    QLabel *chLbl = new QLabel("通道:");
    chLbl->setStyleSheet("color: #aaa; font-size: 12px;");
    toolLay->addWidget(chLbl);

    filterChannel_ = new QComboBox();
    filterChannel_->addItem("全部", -1);  // 全部通道
    filterChannel_->addItem("通道 1", 0);  // 通道1
    filterChannel_->addItem("通道 2", 1);  // 通道2
    filterChannel_->addItem("通道 3", 2);  // 通道3
    filterChannel_->addItem("通道 4", 3);  // 通道4
    filterChannel_->setStyleSheet("color: #fff; background: #3d3d4d; border: 1px solid #555; border-radius: 4px; padding: 4px 8px;");
    toolLay->addWidget(filterChannel_);

    // 类别筛选标签和下拉框
    QLabel *clsLbl = new QLabel("类别:");
    clsLbl->setStyleSheet("color: #aaa; font-size: 12px;");
    toolLay->addWidget(clsLbl);

    filterClass_ = new QComboBox();
    filterClass_->addItem("全部", "");  // 全部类别
    filterClass_->setStyleSheet("color: #fff; background: #3d3d4d; border: 1px solid #555; border-radius: 4px; padding: 4px 8px;");
    toolLay->addWidget(filterClass_);

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
    告警表格
    说明：显示告警详细信息，支持筛选和操作
    ====================================================
    */
    table_ = new QTableWidget();
    table_->setColumnCount(6);  // 6列
    table_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "状态", "ID"});
    table_->horizontalHeader()->setStretchLastSection(true);  // 最后一列拉伸
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);  // 时间列拉伸
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);  // 选择整行
    table_->setAlternatingRowColors(true);  // 交替行颜色
    table_->setStyleSheet(
        "QTableWidget { background: #1e1e2e; color: #ccc; gridline-color: #333; }"
        "QTableWidget::item:selected { background: #3d5a80; }"
        "QHeaderView::section { background: #2d2d3d; color: #aaa; padding: 6px; border: 1px solid #333; }"
    );
    table_->verticalHeader()->setVisible(false);  // 隐藏行号
    
    // 双击行事件处理
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &AlarmListWidget::onRowDoubleClicked);
    mainLay->addWidget(table_, 1);  // 表格占据剩余空间
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

    // 获取所有告警记录
    QVector<AlarmRecord> alarms = mgr.alarms();

    // 更新统计信息
    lblTotal_->setText(QString("共 %1 条").arg(alarms.size()));
    int unack = mgr.unacknowledgedCount();
    lblUnack_->setText(QString("未确认: %1 条").arg(unack));
    lblUnack_->setStyleSheet(unack > 0
        ? "color: #ff9800; font-size: 13px;"
        : "color: #4caf50; font-size: 13px;");

    /* 
    ====================================================
    更新类别筛选下拉框
    说明：从告警记录中提取所有类别，填充下拉框
    ====================================================
    */
    QSet<QString> classes;
    for (const auto &a : alarms)
        classes.insert(a.className);  // 收集所有类别
    
    QString currentClass = filterClass_->currentText();  // 记住当前选择
    filterClass_->clear();
    filterClass_->addItem("全部", "");  // 添加"全部"选项
    for (const auto &cls : classes) {
        filterClass_->addItem(cls, cls);  // 添加各个类别
    }
    int idx = filterClass_->findText(currentClass);
    if (idx >= 0) filterClass_->setCurrentIndex(idx);  // 恢复选择

    /* 
    ====================================================
    应用筛选条件
    说明：根据用户选择的通道和类别筛选告警记录
    ====================================================
    */
    int filterCh = filterChannel_->currentData().toInt();  // 获取通道筛选值
    QString filterCls = filterClass_->currentData().toString();  // 获取类别筛选值

    QVector<AlarmRecord> filtered;
    for (const auto &a : alarms) {
        if (filterCh >= 0 && a.channel != filterCh) continue;  // 通道不匹配
        if (!filterCls.isEmpty() && a.className != filterCls) continue;  // 类别不匹配
        filtered.append(a);  // 符合条件
    }

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

        // 时间列
        QDateTime dt;
        dt.setMSecsSinceEpoch(a.timestamp / 1000000);  // 纳秒转换为毫秒
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        timeItem->setData(Qt::UserRole, a.imgPath);  // 存储截图路径
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
        QTableWidgetItem *statusItem = new QTableWidgetItem(
            a.acknowledged ? "已确认" : "未确认");
        statusItem->setForeground(a.acknowledged ? QColor(76, 175, 80) : QColor(255, 152, 0));  // 颜色区分
        table_->setItem(i, 4, statusItem);

        // ID列
        table_->setItem(i, 5, new QTableWidgetItem(QString::number(i)));
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
说明：双击行时用系统看图程序打开该告警的截图
====================================================
*/
void AlarmListWidget::onRowDoubleClicked(int row, int)
{
    // 获取该行的时间单元格（存储了截图路径）
    QTableWidgetItem *it = table_->item(row, 0);
    if (!it) return;
    
    // 获取截图路径
    QString imgPath = it->data(Qt::UserRole).toString();
    if (imgPath.isEmpty() || !QFile::exists(imgPath)) {
        QMessageBox::information(this, "截图", "该报警没有截图记录");
        return;
    }
    
    // 获取绝对路径并打开
    QString abs = QFileInfo(imgPath).absoluteFilePath();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(abs))) {
        QMessageBox::information(this, "截图",
                                 QString("无法打开图片：\n%1").arg(abs));
    }
}