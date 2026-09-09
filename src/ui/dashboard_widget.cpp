#include "dashboard_widget.h"
#include "alarm_manager.h"
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFrame>
#include <QScrollArea>
#include <QFile>
#include <QDateTime>
#include <QTextStream>
#include <QRegExp>

/* 
====================================================
作用：仪表盘构造函数
说明：初始化界面，设置定时刷新机制
====================================================
*/
DashboardWidget::DashboardWidget(QWidget *parent)
    : QWidget(parent)
{
    startTime_ = QDateTime::currentMSecsSinceEpoch();  // 记录启动时间
    setupUi();  // 初始化UI组件
    // 连接定时器信号到刷新槽函数
    connect(&refreshTimer_, &QTimer::timeout, this, &DashboardWidget::onTimer);
    refreshTimer_.start(2000);  // 每2秒刷新一次
    onTimer();  // 立即执行一次刷新
}

/* 
====================================================
作用：创建统计卡片组件
说明：创建带标题、数值、单位和描述的卡片
参数：titleText - 卡片标题，unit - 单位文本
      valueLabel - 数值标签指针的指针（输出参数）
      descLabel - 描述标签指针的指针（输出参数）
返回值：创建的卡片组件
====================================================
*/
QWidget *DashboardWidget::createCard(const QString &titleText, const QString &unit,
                                      QLabel **valueLabel, QLabel **descLabel)
{
    // 创建卡片框架
    QFrame *card = new QFrame();
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(
        "QFrame { background: #2d2d3d; border-radius: 8px; padding: 6px; }"
    );
    card->setMinimumHeight(135);  // 最小高度
    card->setMinimumWidth(240);   // 最小宽度

    // 创建垂直布局
    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(18, 14, 18, 14);  // 边距
    lay->setSpacing(4);  // 间距

    // 创建标题标签
    QLabel *titleLabel = new QLabel(titleText);
    titleLabel->setStyleSheet("color: #9aa0a6; font-size: 15px; font-weight: bold;");
    lay->addWidget(titleLabel);

    // 创建数值显示区域
    QHBoxLayout *valLay = new QHBoxLayout();
    valLay->setSpacing(6);
    *valueLabel = new QLabel("0");  // 初始化数值为0
    (*valueLabel)->setStyleSheet("color: #fff; font-size: 36px; font-weight: bold;");
    (*valueLabel)->setAlignment(Qt::AlignVCenter);  // 垂直居中对齐
    valLay->addWidget(*valueLabel);

    // 如果有单位，添加单位标签
    if (!unit.isEmpty()) {
        QLabel *unitLabel = new QLabel(unit);
        unitLabel->setStyleSheet("color: #666; font-size: 14px;");
        unitLabel->setAlignment(Qt::AlignBottom | Qt::AlignLeft);  // 左下角对齐
        valLay->addWidget(unitLabel);
    }
    valLay->addStretch();  // 添加弹性空间
    lay->addLayout(valLay);

    // 创建描述标签
    *descLabel = new QLabel("");
    (*descLabel)->setStyleSheet("color: #aaa; font-size: 13px;");
    (*descLabel)->setWordWrap(true);  // 启用自动换行
    lay->addWidget(*descLabel);

    return card;
}

/* 
====================================================
作用：设置用户界面
说明：创建仪表盘的所有UI组件和布局
====================================================
*/
void DashboardWidget::setupUi()
{
    // 创建主垂直布局
    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(20, 16, 20, 16);
    mainLay->setSpacing(14);

    // 创建标题标签
    QLabel *header = new QLabel("数据看板");
    header->setStyleSheet("color: #fff; font-size: 22px; font-weight: bold;");
    mainLay->addWidget(header);

    /* 
    ====================================================
    统计卡片区域（4列网格布局）
    说明：显示检测总数、报警次数、在线通道、运行时长
    ====================================================
    */
    QGridLayout *cardGrid = new QGridLayout();
    cardGrid->setSpacing(14);

    // 添加四个统计卡片
    cardGrid->addWidget(
        createCard("检测总数", "次", &lblDetections_, &lblDetectionsDesc_), 0, 0);
    cardGrid->addWidget(
        createCard("报警次数", "条", &lblAlarms_, &lblAlarmsDesc_), 0, 1);
    cardGrid->addWidget(
        createCard("在线通道", "路", &lblChannels_, &lblChannelsDesc_), 0, 2);
    cardGrid->addWidget(
        createCard("运行时长", "", &lblUptime_, &lblUptimeDesc_), 0, 3);
    
    // 设置列拉伸因子，使四列等宽
    cardGrid->setColumnStretch(0, 1);
    cardGrid->setColumnStretch(1, 1);
    cardGrid->setColumnStretch(2, 1);
    cardGrid->setColumnStretch(3, 1);

    mainLay->addLayout(cardGrid);

    /* 
    ====================================================
    系统状态区域
    说明：显示CPU、内存、温度、NPU使用情况
    ====================================================
    */
    QLabel *sysTitle = new QLabel("系统状态");
    sysTitle->setStyleSheet("color: #fff; font-size: 17px; font-weight: bold;");
    mainLay->addWidget(sysTitle);

    QGridLayout *sysGrid = new QGridLayout();
    sysGrid->setSpacing(12);

    // 创建系统状态卡片的Lambda函数
    auto makeStatusCard = [&](const QString &label, QLabel **val, int row, int col) {
        QFrame *frame = new QFrame();
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setStyleSheet(
            "QFrame { background: #2d2d3d; border-radius: 6px; padding: 12px; }");
        frame->setMinimumHeight(52);
        QHBoxLayout *h = new QHBoxLayout(frame);
        h->setContentsMargins(14, 6, 14, 6);
        QLabel *lbl = new QLabel(label);
        lbl->setStyleSheet("color: #aaa; font-size: 14px;");
        h->addWidget(lbl);
        h->addStretch();
        *val = new QLabel("--");  // 初始显示"--"
        (*val)->setStyleSheet("color: #4fc3f7; font-size: 16px; font-weight: bold;");
        h->addWidget(*val);
        sysGrid->addWidget(frame, row, col);
    };

    // 创建四个系统状态卡片
    makeStatusCard("CPU 使用率", &lblCpu_, 0, 0);
    makeStatusCard("内存使用率", &lblMem_, 0, 1);
    makeStatusCard("芯片温度", &lblTemp_, 1, 0);
    makeStatusCard("NPU 状态", &lblNpu_, 1, 1);

    mainLay->addLayout(sysGrid);

    /* 
    ====================================================
    类别统计区域（独立滚动）
    说明：显示检测目标类别的统计信息，支持滚动查看
    ====================================================
    */
    QLabel *clsTitle = new QLabel("类别统计");
    clsTitle->setStyleSheet("color: #fff; font-size: 17px; font-weight: bold;");
    mainLay->addWidget(clsTitle);

    // 创建滚动区域
    classStatsScroll_ = new QScrollArea();
    classStatsScroll_->setWidgetResizable(true);  // 启用组件大小调整
    classStatsScroll_->setFrameShape(QFrame::StyledPanel);
    classStatsScroll_->setStyleSheet(
        "QScrollArea { background: #2d2d3d; border-radius: 6px; border: 1px solid #3d3d4d; }");
    classStatsScroll_->setMinimumHeight(160);  // 最小高度

    // 创建滚动内容组件
    classStatsWidget_ = new QWidget();
    classStatsLayout_ = new QVBoxLayout(classStatsWidget_);
    classStatsLayout_->setContentsMargins(14, 10, 14, 10);
    classStatsLayout_->setSpacing(6);
    classStatsLayout_->addStretch();  // 添加弹性空间

    // 创建空数据提示标签
    QLabel *emptyLabel = new QLabel("暂无数据");
    emptyLabel->setStyleSheet("color: #666; font-size: 13px;");
    classStatsLayout_->insertWidget(0, emptyLabel);

    // 设置滚动组件
    classStatsScroll_->setWidget(classStatsWidget_);
    mainLay->addWidget(classStatsScroll_, 1);   // 占剩余高度，内部滚动
}

/* 
====================================================
作用：定时器槽函数
说明：触发统计信息刷新
====================================================
*/
void DashboardWidget::onTimer()
{
    refreshStats();
}

/* 
====================================================
作用：刷新统计信息
说明：从AlarmManager获取最新数据并更新UI显示
====================================================
*/
void DashboardWidget::refreshStats()
{
    AlarmManager &mgr = AlarmManager::instance();  // 获取告警管理器单例

    // 更新检测总数
    lblDetections_->setText(QString::number(mgr.totalDetections()));
    lblDetectionsDesc_->setText("本次运行累计检测目标数");

    // 更新报警次数
    int alarmCnt = mgr.alarmCount();
    lblAlarms_->setText(QString::number(alarmCnt));
    int unack = mgr.unacknowledgedCount();  // 获取未确认告警数
    lblAlarmsDesc_->setText(unack > 0 ? QString("其中 %1 条未确认").arg(unack) : "全部已确认");

    // 更新在线通道数
    int online = mgr.onlineChannelCount();
    lblChannels_->setText(QString::number(online));
    lblChannelsDesc_->setText(online > 0 ? "正在解码的视频通道" : "暂无通道在线");

    // 计算并显示运行时长
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startTime_;
    int sec = (int)(elapsed / 1000) % 60;  // 秒
    int min = (int)(elapsed / 60000) % 60;  // 分钟
    int hr  = (int)(elapsed / 3600000);     // 小时
    // 格式化为 HH:MM:SS
    lblUptime_->setText(QString("%1:%2:%3")
        .arg(hr, 2, 10, QChar('0'))   // 小时，两位数，前导零
        .arg(min, 2, 10, QChar('0'))  // 分钟，两位数，前导零
        .arg(sec, 2, 10, QChar('0'))); // 秒，两位数，前导零
    lblUptimeDesc_->setText("自程序启动以来");

    /* 
    ====================================================
    系统状态监控
    说明：读取Linux系统文件获取CPU、内存、温度信息
    ====================================================
    */
    
    // CPU使用率监控
    // 通过读取/proc/stat文件获取CPU时间信息
    static qint64 lastTotal = 0, lastIdle = 0;
    QFile cpuFile("/proc/stat");
    if (cpuFile.open(QIODevice::ReadOnly)) {
        QByteArray line = cpuFile.readLine();  // 读取第一行（CPU总时间）
        cpuFile.close();
        QList<QByteArray> parts = line.split(' ');
        if (parts.size() >= 5) {
            qint64 total = 0;
            // 累加所有CPU时间（用户、nice、系统、空闲等）
            for (int i = 1; i < parts.size() && i <= 10; i++)
                total += parts[i].toLongLong();
            qint64 idle = parts[4].toLongLong();  // 空闲时间
            qint64 dTotal = total - lastTotal;    // 时间增量
            qint64 dIdle = idle - lastIdle;       // 空闲增量
            lastTotal = total;
            lastIdle = idle;
            // 计算CPU使用率百分比
            int usage = (dTotal > 0) ? (int)((dTotal - dIdle) * 100 / dTotal) : 0;
            lblCpu_->setText(QString("%1%").arg(usage));
        }
    }

    // 内存使用率监控
    // 通过读取/proc/meminfo文件获取内存信息
    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly)) {
        qint64 total = 0, avail = 0;
        while (!memFile.atEnd()) {
            QByteArray line = memFile.readLine();
            if (line.startsWith("MemTotal:"))
                total = QByteArray(line).split(' ')[1].toLongLong();  // 总内存
            else if (line.startsWith("MemAvailable:"))
                avail = QByteArray(line).split(' ')[1].toLongLong();  // 可用内存
        }
        memFile.close();
        if (total > 0) {
            // 计算内存使用率百分比
            int usage = (int)((total - avail) * 100 / total);
            lblMem_->setText(QString("%1%").arg(usage));
        }
    }

    // 温度监控
    // 通过读取sysfs文件获取芯片温度
    QFile tempFile("/sys/class/thermal/thermal_zone0/temp");
    if (tempFile.open(QIODevice::ReadOnly)) {
        int temp = tempFile.readAll().trimmed().toInt();  // 读取温度值（毫摄氏度）
        tempFile.close();
        // 转换为摄氏度并格式化显示
        lblTemp_->setText(QString("%1°C").arg(temp / 1000.0, 0, 'f', 1));
    }

    // NPU状态（硬编码为3核在线）
    lblNpu_->setText("3 核在线");

    /* 
    ====================================================
    类别统计更新
    说明：从AlarmManager获取类别统计，按数量排序显示
    ====================================================
    */
    QMap<QString, int> stats = mgr.classStatistics();

    // 清空旧的统计内容
    QLayoutItem *item;
    while ((item = classStatsLayout_->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (stats.isEmpty()) {
        // 没有数据时显示提示
        QLabel *emptyLabel = new QLabel("暂无数据");
        emptyLabel->setStyleSheet("color: #666; font-size: 12px;");
        classStatsLayout_->addWidget(emptyLabel);
    } else {
        // 按数量排序统计结果
        QList<QPair<QString, int>> sorted;
        for (auto it = stats.begin(); it != stats.end(); ++it)
            sorted.append(qMakePair(it.key(), it.value()));
        // 降序排序
        std::sort(sorted.begin(), sorted.end(),
                  [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                      return a.second > b.second;
                  });

        // 为每个类别创建显示行
        for (const auto &pair : sorted) {
            QWidget *row = new QWidget();
            QHBoxLayout *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 2, 0, 2);

            // 类别名称标签
            QLabel *nameLbl = new QLabel(pair.first);
            nameLbl->setStyleSheet("color: #ccc; font-size: 12px;");
            nameLbl->setFixedWidth(120);
            h->addWidget(nameLbl);

            // 文本进度条（使用█字符）
            int barLen = (pair.second * 30) / (sorted.first().second > 0 ? sorted.first().second : 1);
            QString barText = QString("█").repeated(barLen);
            QLabel *barLbl = new QLabel(barText);
            barLbl->setStyleSheet("color: #4fc3f7; font-size: 12px;");
            h->addWidget(barLbl, 1);

            // 数量标签
            QLabel *countLbl = new QLabel(QString::number(pair.second));
            countLbl->setStyleSheet("color: #aaa; font-size: 12px;");
            countLbl->setFixedWidth(60);
            countLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(countLbl);

            classStatsLayout_->addWidget(row);
        }
    }
}