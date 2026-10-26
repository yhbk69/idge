#include "video_alarm_widget.h"
#include "alarm_detail_dialog.h"
#include "theme.h"
#include "qt_image_utils.h"
#include "perf_probe.h"
#include <QScrollArea>
#include <QElapsedTimer>
#include <QShowEvent>
#include <QDebug>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPixmap>
#include <QDateTime>
#include <QFile>
#include <QCursor>
#include <QMessageBox>
#include <QScreen>
#include <QGuiApplication>

VideoAlarmWidget::VideoAlarmWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

void VideoAlarmWidget::setupUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 标题
    QLabel *title = new QLabel("报警信息");
    title->setStyleSheet(QString("color: %1; font-size:%2px; font-weight: bold; padding: 8px 12px; background: %3;")
                             .arg(theme::TEXT).arg(theme::FS_BODY).arg(theme::PANEL));
    mainLayout->addWidget(title);

    // 滚动区域
    scrollArea_ = new QScrollArea();
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);
    scrollArea_->setStyleSheet(QString("QScrollArea { background: %1; border: none; }").arg(theme::BG));

    contentWidget_ = new QWidget();
    // 条目样式集中在父级设一次，条目内控件只用 objectName 命中规则：QSS 每次
    // setStyleSheet 都要重新解析样式表文本并重新 polish 子树，一条报警 6 个控件、
    // 20 条就是 120 次解析。实测条目构造本身只剩 3~5ms，大头转移到了抓拍图解码。
    contentWidget_->setStyleSheet(QString(
        "background: %1;"
        "QWidget#alarmItem { background: %2; border-radius: 6px; }"
        "QWidget#alarmItem QLabel { background: %2; }"
        "QWidget#alarmItem QLabel#alarmThumb { background: %1; border: 1px solid %3; border-radius: 4px; }"
        "QLabel#alarmTime, QLabel#alarmChannel { color: %4; font-size: %5px; }"
        "QLabel#alarmClass { color: %6; font-size: %5px; font-weight: bold; }"
        "QPushButton#alarmDetail { color: %6; background: %7; border-radius: 4px;"
        " padding: 3px 8px; font-size: %5px; }"
        "QPushButton#alarmDetail:hover { background: %8; }")
                                     .arg(theme::BG, theme::PANEL, theme::BORDER,
                                          theme::TEXT_MUTED)
                                     .arg(theme::FS_HINT)
                                     .arg(theme::TEXT, theme::BTN_SECONDARY,
                                          theme::BTN_SECONDARY_HOVER));
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(8, 8, 8, 8);
    contentLayout_->setSpacing(8);
    contentLayout_->addStretch();

    scrollArea_->setWidget(contentWidget_);
    mainLayout->addWidget(scrollArea_, 1);
}

void VideoAlarmWidget::refreshAlarms()
{
    // 面板不可见（切到别的页面、窗口被隐藏）时重建毫无意义，而这件事发生在 GUI
    // 单线程上：一次全量重建实测 600~765ms，会把 4 路视频的 paintGL 挤掉 8~10 帧。
    // 隐藏期间到达的报警由 showEvent() 补刷。
    if (!isVisible()) return;

    QElapsedTimer probe;
    probe.start();

    // 权威集合：过滤误报后取最近 20 条，新→旧（与历史显示语义一致）
    const QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();
    const qint64 fetchMs = probe.elapsed();
    QVector<AlarmRecord> wantedRecords;
    wantedRecords.reserve(kMaxDisplay);
    QStringList wantedIds;
    wantedIds.reserve(kMaxDisplay);
    for (int i = alarms.size() - 1; i >= 0 && wantedRecords.size() < kMaxDisplay; --i) {
        if (alarms[i].status == "false_alarm") continue;
        wantedRecords.append(alarms[i]);
        wantedIds.append(alarms[i].id);
    }

    // 集合与顺序都没变 → 直接返回。没有新报警的刷新只剩上面那次 alarms() 拷贝
    // （1000 条内存记录，实测 1~3ms），这条路径是稳态下的绝大多数。
    if (wantedIds == shownIds_) return;

    // 移除已不在集合内的项：被标误报、或被更新的报警挤出 top-20
    int removedCount = 0;
    for (const QString &id : shownIds_) {
        if (wantedIds.contains(id)) continue;
        QWidget *w = items_.take(id);
        if (w) {
            contentLayout_->removeWidget(w);
            delete w;
            ++removedCount;
        }
    }

    // 只给新报警建 widget 并按 wanted 下标升序插入，已有项原地不动。
    // 依据：alarms() 按时间顺序追加，过滤只会删项不会改序，所以已有项在 wanted 里的
    // 相对顺序和它们在布局里的相对顺序恒等；升序把新项插到各自目标下标后，
    // 最终序列与 wanted 完全一致。这样每次刷新只剩"新报警那几次"布局插入
    // （原先固定 20 次，每次都是一回 QScrollArea(setWidgetResizable) 布局失效）。
    // 注意 contentLayout_ 末尾那条 addStretch 占位项要一直留在最后：插入下标最大只有
    // wanted.size()-1，而 stretch 位于所有条目之后，因此永不被越过。
    qint64 createMs = 0;
    int createdCount = 0;
    for (int i = 0; i < wantedRecords.size(); ++i) {
        const AlarmRecord &a = wantedRecords[i];
        if (items_.contains(a.id)) continue;
        QElapsedTimer createProbe;
        createProbe.start();
        contentLayout_->insertWidget(i, createAlarmItem(a));  // 内部登记进 items_
        createMs += createProbe.elapsed();
        ++createdCount;
    }
    shownIds_ = wantedIds;

    if (perfProbe() && probe.elapsed() > 5) {
        qWarning() << "[PROBE] refreshAlarms" << probe.elapsed() << "ms items" << wantedIds.size()
                   << "fetch" << fetchMs << "removed" << removedCount
                   << "created" << createdCount << createMs << "ms";
    }
}

void VideoAlarmWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 隐藏期间的报警不刷新，回到本页时补一次
    refreshAlarms();
}

QWidget *VideoAlarmWidget::createAlarmItem(const AlarmRecord &alarm)
{
    // 样式一律靠 objectName 命中 setupUi() 里那份集中样式表，
    // 子控件上不要再 setStyleSheet（见那里的注释）
    QWidget *itemWidget = new QWidget();
    itemWidget->setObjectName("alarmItem");
    itemWidget->setFixedHeight(80);

    QHBoxLayout *itemLayout = new QHBoxLayout(itemWidget);
    itemLayout->setContentsMargins(8, 6, 8, 6);
    itemLayout->setSpacing(8);

    // 截图缩略图：走 OpenCV 的 DCT 域 1/8 降采样 + 按路径缓存，
    // 不要在这里用 QPixmap(path) 整幅解码 1280x720（见 loadThumbnailScaled 注释）
    QLabel *imageLabel = new QLabel();
    imageLabel->setObjectName("alarmThumb");
    imageLabel->setFixedSize(64, 48);
    imageLabel->setAlignment(Qt::AlignCenter);

    if (!alarm.imagePath.isEmpty() && QFile::exists(alarm.imagePath)) {
        QElapsedTimer thumbProbe;
        thumbProbe.start();
        QPixmap thumb = loadThumbnailScaled(alarm.imagePath, 64, 48);
        const qint64 thumbMs = thumbProbe.elapsed();
        if (perfProbe() && thumbMs > 3) {
            qWarning() << "[PROBE] thumbnail" << thumbMs << "ms" << alarm.imagePath;
        }
        if (!thumb.isNull()) {
            imageLabel->setPixmap(thumb);
        } else {
            imageLabel->setText("无图");
        }
    } else {
        imageLabel->setText("无图");
    }
    itemLayout->addWidget(imageLabel);

    // 信息区域
    QVBoxLayout *infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);

    QDateTime dt = QDateTime::fromString(alarm.alarmTime, Qt::ISODate);
    QLabel *timeLabel = new QLabel(dt.toString("HH:mm:ss"));
    timeLabel->setObjectName("alarmTime");
    infoLayout->addWidget(timeLabel);

    QLabel *classLabel = new QLabel(QString("%1 (%2%)")
        .arg(alarm.className)
        .arg(alarm.confidence * 100, 0, 'f', 0));
    classLabel->setObjectName("alarmClass");
    infoLayout->addWidget(classLabel);

    QLabel *channelLabel = new QLabel(QString("通道%1").arg(alarm.channel + 1));
    channelLabel->setObjectName("alarmChannel");
    infoLayout->addWidget(channelLabel);

    itemLayout->addLayout(infoLayout, 1);

    // 操作按钮（详情）
    QPushButton *btnDetail = new QPushButton("详情");
    btnDetail->setObjectName("alarmDetail");
    connect(btnDetail, &QPushButton::clicked, this, [this, alarmId = alarm.id]() {
        onAlarmClicked(alarmId);
    });
    itemLayout->addWidget(btnDetail);

    items_.insert(alarm.id, itemWidget);
    return itemWidget;
}

void VideoAlarmWidget::onAlarmClicked(const QString &alarmId)
{
    QVector<AlarmRecord> alarms = AlarmManager::instance().alarms();

    AlarmRecord alarm;
    bool found = false;
    for (const auto &a : alarms) {
        if (a.id == alarmId) {
            alarm = a;
            found = true;
            break;
        }
    }
    if (!found) return;

    AlarmDetailDialog dlg(alarm, this);
    connect(&dlg, &AlarmDetailDialog::alarmMarkedFalsePositive, this, [](const QString &id) {
        AlarmManager::instance().markAsFalsePositiveById(id);
    });
    connect(&dlg, &AlarmDetailDialog::alarmAcknowledged, this, [](const QString &id) {
        AlarmManager::instance().acknowledgeAlarmById(id);
    });
    dlg.exec();

    refreshAlarms();
    emit alarmUpdated();
}
