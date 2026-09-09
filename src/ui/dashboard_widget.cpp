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

DashboardWidget::DashboardWidget(QWidget *parent)
    : QWidget(parent)
{
    startTime_ = QDateTime::currentMSecsSinceEpoch();
    setupUi();
    connect(&refreshTimer_, &QTimer::timeout, this, &DashboardWidget::onTimer);
    refreshTimer_.start(2000);
    onTimer();
}

QWidget *DashboardWidget::createCard(const QString &titleText, const QString &unit,
                                      QLabel **valueLabel, QLabel **descLabel)
{
    QFrame *card = new QFrame();
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(
        "QFrame { background: #2d2d3d; border-radius: 8px; padding: 12px; }"
    );
    card->setMinimumHeight(100);

    QVBoxLayout *lay = new QVBoxLayout(card);
    lay->setContentsMargins(16, 12, 16, 12);

    QLabel *titleLabel = new QLabel(titleText);
    titleLabel->setStyleSheet("color: #888; font-size: 12px;");
    lay->addWidget(titleLabel);

    QHBoxLayout *valLay = new QHBoxLayout();
    *valueLabel = new QLabel("0");
    (*valueLabel)->setStyleSheet("color: #fff; font-size: 28px; font-weight: bold;");
    valLay->addWidget(*valueLabel);

    QLabel *unitLabel = new QLabel(unit);
    unitLabel->setStyleSheet("color: #666; font-size: 13px; margin-top: 8px;");
    valLay->addWidget(unitLabel);
    valLay->addStretch();
    lay->addLayout(valLay);

    *descLabel = new QLabel("");
    (*descLabel)->setStyleSheet("color: #aaa; font-size: 11px;");
    lay->addWidget(*descLabel);

    return card;
}

void DashboardWidget::setupUi()
{
    QScrollArea *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setStyleSheet("QScrollArea { background: #1e1e2e; border: none; }");

    QWidget *container = new QWidget();
    QVBoxLayout *mainLay = new QVBoxLayout(container);
    mainLay->setContentsMargins(20, 20, 20, 20);
    mainLay->setSpacing(16);

    // 标题
    QLabel *header = new QLabel("数据看板");
    header->setStyleSheet("color: #fff; font-size: 20px; font-weight: bold;");
    mainLay->addWidget(header);

    // 统计卡片（4 列）
    QGridLayout *cardGrid = new QGridLayout();
    cardGrid->setSpacing(12);

    cardGrid->addWidget(
        createCard("检测总数", "次", &lblDetections_, &lblDetectionsDesc_), 0, 0);
    cardGrid->addWidget(
        createCard("报警次数", "条", &lblAlarms_, &lblAlarmsDesc_), 0, 1);
    cardGrid->addWidget(
        createCard("在线通道", "路", &lblChannels_, &lblChannelsDesc_), 0, 2);
    cardGrid->addWidget(
        createCard("运行时长", "", &lblUptime_, &lblUptimeDesc_), 0, 3);

    mainLay->addLayout(cardGrid);

    // 系统状态
    QLabel *sysTitle = new QLabel("系统状态");
    sysTitle->setStyleSheet("color: #fff; font-size: 16px; font-weight: bold;");
    mainLay->addWidget(sysTitle);

    QGridLayout *sysGrid = new QGridLayout();
    sysGrid->setSpacing(12);

    auto makeStatusCard = [&](const QString &label, QLabel **val, int row, int col) {
        QFrame *frame = new QFrame();
        frame->setFrameShape(QFrame::StyledPanel);
        frame->setStyleSheet(
            "QFrame { background: #2d2d3d; border-radius: 6px; padding: 10px; }");
        QHBoxLayout *h = new QHBoxLayout(frame);
        QLabel *lbl = new QLabel(label);
        lbl->setStyleSheet("color: #aaa; font-size: 12px;");
        h->addWidget(lbl);
        h->addStretch();
        *val = new QLabel("--");
        (*val)->setStyleSheet("color: #4fc3f7; font-size: 14px; font-weight: bold;");
        h->addWidget(*val);
        sysGrid->addWidget(frame, row, col);
    };

    makeStatusCard("CPU 使用率", &lblCpu_, 0, 0);
    makeStatusCard("内存使用率", &lblMem_, 0, 1);
    makeStatusCard("芯片温度", &lblTemp_, 1, 0);
    makeStatusCard("NPU 状态", &lblNpu_, 1, 1);

    mainLay->addLayout(sysGrid);

    // 类别统计
    QLabel *clsTitle = new QLabel("类别统计");
    clsTitle->setStyleSheet("color: #fff; font-size: 16px; font-weight: bold;");
    mainLay->addWidget(clsTitle);

    classStatsWidget_ = new QFrame();
    classStatsWidget_->setFrameShape(QFrame::StyledPanel);
    classStatsWidget_->setStyleSheet(
        "QFrame { background: #2d2d3d; border-radius: 6px; padding: 12px; }");
    classStatsLayout_ = new QVBoxLayout(classStatsWidget_);
    classStatsLayout_->setContentsMargins(12, 8, 12, 8);
    classStatsLayout_->setSpacing(4);

    QLabel *emptyLabel = new QLabel("暂无数据");
    emptyLabel->setStyleSheet("color: #666; font-size: 12px;");
    classStatsLayout_->addWidget(emptyLabel);

    mainLay->addWidget(classStatsWidget_);
    mainLay->addStretch();

    scroll->setWidget(container);

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
}

void DashboardWidget::onTimer()
{
    refreshStats();
}

void DashboardWidget::refreshStats()
{
    AlarmManager &mgr = AlarmManager::instance();

    // 检测总数
    lblDetections_->setText(QString::number(mgr.totalDetections()));
    lblDetectionsDesc_->setText("本次运行累计检测目标数");

    // 报警次数
    int alarmCnt = mgr.alarmCount();
    lblAlarms_->setText(QString::number(alarmCnt));
    int unack = mgr.unacknowledgedCount();
    lblAlarmsDesc_->setText(unack > 0 ? QString("其中 %1 条未确认").arg(unack) : "全部已确认");

    // 在线通道（读解码器真实状态）
    int online = mgr.onlineChannelCount();
    lblChannels_->setText(QString::number(online));
    lblChannelsDesc_->setText(online > 0 ? "正在解码的视频通道" : "暂无通道在线");

    // 运行时长
    qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startTime_;
    int sec = (int)(elapsed / 1000) % 60;
    int min = (int)(elapsed / 60000) % 60;
    int hr  = (int)(elapsed / 3600000);
    lblUptime_->setText(QString("%1:%2:%3")
        .arg(hr, 2, 10, QChar('0'))
        .arg(min, 2, 10, QChar('0'))
        .arg(sec, 2, 10, QChar('0')));
    lblUptimeDesc_->setText("自程序启动以来");

    // ===== 系统状态 =====
    // CPU
    static qint64 lastTotal = 0, lastIdle = 0;
    QFile cpuFile("/proc/stat");
    if (cpuFile.open(QIODevice::ReadOnly)) {
        QByteArray line = cpuFile.readLine();
        cpuFile.close();
        QList<QByteArray> parts = line.split(' ');
        if (parts.size() >= 5) {
            qint64 total = 0;
            for (int i = 1; i < parts.size() && i <= 10; i++)
                total += parts[i].toLongLong();
            qint64 idle = parts[4].toLongLong();
            qint64 dTotal = total - lastTotal;
            qint64 dIdle = idle - lastIdle;
            lastTotal = total;
            lastIdle = idle;
            int usage = (dTotal > 0) ? (int)((dTotal - dIdle) * 100 / dTotal) : 0;
            lblCpu_->setText(QString("%1%").arg(usage));
        }
    }

    // Memory
    QFile memFile("/proc/meminfo");
    if (memFile.open(QIODevice::ReadOnly)) {
        qint64 total = 0, avail = 0;
        while (!memFile.atEnd()) {
            QByteArray line = memFile.readLine();
            if (line.startsWith("MemTotal:"))
                total = QByteArray(line).split(' ')[1].toLongLong();
            else if (line.startsWith("MemAvailable:"))
                avail = QByteArray(line).split(' ')[1].toLongLong();
        }
        memFile.close();
        if (total > 0) {
            int usage = (int)((total - avail) * 100 / total);
            lblMem_->setText(QString("%1%").arg(usage));
        }
    }

    // Temperature
    QFile tempFile("/sys/class/thermal/thermal_zone0/temp");
    if (tempFile.open(QIODevice::ReadOnly)) {
        int temp = tempFile.readAll().trimmed().toInt();
        tempFile.close();
        lblTemp_->setText(QString("%1°C").arg(temp / 1000.0, 0, 'f', 1));
    }

    // NPU
    lblNpu_->setText("3 核在线");

    // ===== 类别统计 =====
    QMap<QString, int> stats = mgr.classStatistics();

    // 清空旧内容
    QLayoutItem *item;
    while ((item = classStatsLayout_->takeAt(0)) != nullptr) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    if (stats.isEmpty()) {
        QLabel *emptyLabel = new QLabel("暂无数据");
        emptyLabel->setStyleSheet("color: #666; font-size: 12px;");
        classStatsLayout_->addWidget(emptyLabel);
    } else {
        QList<QPair<QString, int>> sorted;
        for (auto it = stats.begin(); it != stats.end(); ++it)
            sorted.append(qMakePair(it.key(), it.value()));
        std::sort(sorted.begin(), sorted.end(),
                  [](const QPair<QString, int> &a, const QPair<QString, int> &b) {
                      return a.second > b.second;
                  });

        for (const auto &pair : sorted) {
            QWidget *row = new QWidget();
            QHBoxLayout *h = new QHBoxLayout(row);
            h->setContentsMargins(0, 2, 0, 2);

            QLabel *nameLbl = new QLabel(pair.first);
            nameLbl->setStyleSheet("color: #ccc; font-size: 12px;");
            nameLbl->setFixedWidth(120);
            h->addWidget(nameLbl);

            // 简单的文本进度条
            int barLen = (pair.second * 30) / (sorted.first().second > 0 ? sorted.first().second : 1);
            QString barText = QString("█").repeated(barLen);
            QLabel *barLbl = new QLabel(barText);
            barLbl->setStyleSheet("color: #4fc3f7; font-size: 12px;");
            h->addWidget(barLbl, 1);

            QLabel *countLbl = new QLabel(QString::number(pair.second));
            countLbl->setStyleSheet("color: #aaa; font-size: 12px;");
            countLbl->setFixedWidth(60);
            countLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(countLbl);

            classStatsLayout_->addWidget(row);
        }
    }
}
