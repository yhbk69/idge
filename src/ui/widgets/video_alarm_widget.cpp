// ============================================================================
// video_alarm_widget.cpp - 视频监控页右侧"最近报警"卡片面板实现
// ============================================================================
//
// 【所属模块】src/ui/widgets。数据源是 AlarmManager 的**内存窗口**（最近 1000 条
//   里取最新 20 条，滤掉误报），不是数据库 —— 全量历史检索在 alarm_list_widget。
// 【一条不变式】items_ 的 key 集合 ≡ shownIds_，且 shownIds_ 的顺序就是布局里
//   从上到下的顺序（新→旧）。refreshAlarms() 的全部工作就是让这个不变式重新成立。
//   注意判重比的是**id 序列**：卡片内容变了而 id 集合没变（比如类别名被回填）不会
//   重绘——目前成立是因为卡片显示的类别/置信度/时间/通道在报警产生后就不再改，
//   状态变化（确认/误报）里只有"误报"会动这个集合（被过滤掉→id 少一个）。
// 【耗时口径】稳态一次刷新 = 一次 alarms() 快照拷贝（1~3ms）；
//   全拆重建实测 600~765ms（会挤掉四路 paintGL 8~10 帧），增量化之后剩下的
//   大头是抓拍图解码 —— 所以缩略图必须走 loadThumbnailScaled，不能用 QPixmap(path)。
// 【线程】全部 GUI 线程；没有定时器，刷新时机由外部（frmMain 的 300ms 防抖）驱动。
// ============================================================================
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
#include <QMouseEvent>
#include <QVariant>
#include <QPixmap>
#include <QDateTime>
#include <QFile>
#include <QCursor>
#include <QMessageBox>
#include <QScreen>
#include <QGuiApplication>

// 构造只做骨架（setupUi），**不查库**：本面板建好时可能一直不被切到，
// 第一次数据由 showEvent 带进来（外部防抖调 refreshAlarms 时也会被 isVisible() 挡）。
VideoAlarmWidget::VideoAlarmWidget(QWidget *parent)
    : QWidget(parent)
{
    setupUi();
}

// 骨架：标题 + QScrollArea + 一个装卡片的 contentWidget。
// 布局末尾那条 addStretch() 是**必须的**：卡片少于 20 张时它把条目顶在上面，
// 否则多出来的竖向空间会被均分成条目之间的空档（卡片是 setFixedHeight，
// 自己不会长大，但会在各自分到的格子里居中）。
// refreshAlarms 的 insertWidget(i, …) 下标也默认这条 stretch 恒在末尾，删掉它就会错位。
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
    // setStyleSheet 都要重新解析样式表文本并重新 polish 子树，一条报警 3 个控件、
    // 20 条就是 60 次解析。实测条目构造本身只剩 3~5ms，大头转移到了抓拍图解码。
    //
    // 条目里的 QLabel 一律 transparent 底：卡片整张是点击入口后，悬停/按下的高亮
    // 必须看得出来 —— 标签各自刷成 PANEL 的话，卡片换色只会在 8px 的缝隙里露一点，
    // 等于没有反馈。（缩略图那条更具体，仍然保留自己的底。）
    contentWidget_->setStyleSheet(QString(
        "background: %1;"
        "QWidget#alarmItem { background: %2; border-radius: 6px; }"
        "QWidget#alarmItem:hover { background: %8; }"
        "QWidget#alarmItem QLabel { background: transparent; }"
        "QWidget#alarmItem QLabel#alarmThumb { background: %1; border: 1px solid %3; border-radius: 4px; }"
        "QLabel#alarmChannel { color: %4; font-size: %5px; }"
        "QLabel#alarmClass { color: %6; font-size: %5px; font-weight: bold; }")
                                     .arg(theme::BG, theme::PANEL, theme::BORDER,
                                          theme::TEXT_MUTED)
                                     .arg(theme::FS_HINT)
                                     .arg(theme::TEXT, theme::HOVER));   // 卡片底用"深悬停底"，不是按钮那一档亮蓝
    contentLayout_ = new QVBoxLayout(contentWidget_);
    contentLayout_->setContentsMargins(8, 8, 8, 8);
    contentLayout_->setSpacing(8);
    contentLayout_->addStretch();

    scrollArea_->setWidget(contentWidget_);
    mainLayout->addWidget(scrollArea_, 1);
}

// 增量对齐（详见文件头【一条不变式】）。三步的顺序不能换：
//   算 wanted（倒着扫内存窗口，滤误报，攒够 20 就停）→ 与 shownIds_ 比，一样就 return
//   → 删消失的、建新增的。倒着扫是因为 alarms() 是时间升序追加的，
//   最新在末尾，所以这里从 size()-1 往前取，取出来天然就是"新→旧"。
// 只在新报警真的到来时才付建 widget 的代价；已存在的条目一律原地不动。
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

// 这是隐藏期间漏掉的那次刷新唯一的补口：不可见时 refreshAlarms 直接 return，
// 外部再怎么防抖都不会把数据带进来，全靠这里补一次。
void VideoAlarmWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 隐藏期间的报警不刷新，回到本页时补一次
    refreshAlarms();
}

// 建一张卡片：缩略图 + 两行文字，登记进 items_ 后交给调用方落位。
// 调用约束：只在 refreshAlarms 里被调（那里负责 insert 进布局并保证同一 id 只建一次），
// 别处直接调会得到一张"已登记但不在布局里"的隐形卡片 —— items_ 判重会把它当已存在，
// 于是这条报警在界面上永远不出现。
// 内存：parent 为空，靠布局持有；refreshAlarms 删除时是 removeWidget + delete 成对做。
QWidget *VideoAlarmWidget::createAlarmItem(const AlarmRecord &alarm)
{
    // 样式一律靠 objectName 命中 setupUi() 里那份集中样式表，
    // 子控件上不要再 setStyleSheet（见那里的注释）
    QWidget *itemWidget = new QWidget();
    itemWidget->setObjectName("alarmItem");
    // 104→72：撤掉「详情」钮后卡片只剩两行文字，竖向再没有别的东西要地方。
    // 侧栏列宽被 .ui 锁死 280px，横向才是稀缺的；竖向省下来的是"一屏能看几条"，
    // 实测同一块侧栏从 ~8 条抬到 ~12 条。
    itemWidget->setFixedHeight(72);
    // 整张卡片就是入口：光标 + 悬停变色。:hover 对普通 QWidget 要 WA_Hover 才发事件。
    itemWidget->setCursor(Qt::PointingHandCursor);
    itemWidget->setAttribute(Qt::WA_Hover, true);
    itemWidget->setProperty("alarmId", alarm.id);
    itemWidget->installEventFilter(this);

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

    // 信息区域：两行，先"是什么"再"什么时候/哪一路"。
    // 负责人这一项从卡片上撤了 —— 它是**会话级**的常量（makeAlarmRecord 从当前登录
    // 打戳），同一次登录里每条报警都是同一个名字，而标题栏右上角那颗角标恒显示的就是
    // 这个姓名；280px 的侧栏里它是纯粹占掉类别名那一行的宽度。要看的人：报警数据页
    // 有负责人列，点开这条的详情框有整份施工信息。
    QVBoxLayout *infoLayout = new QVBoxLayout();
    infoLayout->setSpacing(2);

    QLabel *classLabel = new QLabel(QString("%1 %2%")
        .arg(alarm.className)
        .arg(alarm.confidence * 100, 0, 'f', 0));
    classLabel->setObjectName("alarmClass");
    infoLayout->addWidget(classLabel);

    QDateTime dt = QDateTime::fromString(alarm.alarmTime, Qt::ISODate);
    QLabel *metaLabel = new QLabel(QString("%1 · 通道%2")
                                       .arg(dt.toString("HH:mm:ss"))
                                       .arg(alarm.channel + 1));
    metaLabel->setObjectName("alarmChannel");   // 与原来同一档字号/颜色，不新增样式
    infoLayout->addWidget(metaLabel);

    itemLayout->addLayout(infoLayout, 1);

    // 子件一律不吃鼠标：否则点在文字或缩略图上就不会冒到卡片，
    // "整张可点"会变成"只有那圈空白可点"。
    for (QWidget *child : itemWidget->findChildren<QWidget *>()) {
        child->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    items_.insert(alarm.id, itemWidget);
    return itemWidget;
}

// ============================================================================
// eventFilter - 卡片点击 → 打开这条报警的详情
// ============================================================================
// 为什么要自己判位移而不直接认 release：这块侧栏挂了 QScroller 触摸拖动
// （frmMain 构造末尾的 enableTouchScrollAll），一次滑动的收尾同样是落在卡片上的
// MouseButtonRelease —— 不判位移就是"每次滚动列表都弹一个详情框"。
// 阈值 16px ≈ 1.8mm（板上 226PPI），够容指肚的滚动，又远小于一次有意的滑动。
// ============================================================================
bool VideoAlarmWidget::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *card = qobject_cast<QWidget *>(watched);
    if (card) {
        const QMouseEvent *me = static_cast<const QMouseEvent *>(event);
        if (event->type() == QEvent::MouseButtonPress && me->button() == Qt::LeftButton) {
            card->setProperty("pressPos", me->globalPos());
        } else if (event->type() == QEvent::MouseButtonRelease && me->button() == Qt::LeftButton) {
            const QPoint down = card->property("pressPos").toPoint();
            card->setProperty("pressPos", QVariant());
            const QString id = card->property("alarmId").toString();
            if (!id.isEmpty() && (me->globalPos() - down).manhattanLength() <= 16) {
                onAlarmClicked(id);
                return true;   // 这次点击已经被"打开详情"消费掉了
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// 卡片点击 → 详情框。三点值得知道：
//   1) 按 id 在**内存窗口**里找（不是库）：卡片本来就来自这份快照，
//      找不到只有一种情况——这条已被裁出最近 1000 条，此时静默返回（不弹错框，
//      现场"点了没反应"通常是这张卡片已经过期了一瞬）。
//   2) dlg 是栈上对象 + exec()：GUI 线程在模态循环里**仍然**会跑防抖定时器，
//      但本面板的 refreshAlarms 会照常被调（本窗体此时仍可见），这是可接受的重复劳动。
//   3) 两条 connect 是本面板与 AlarmManager 之间唯一的写路径：详情框自己不认识
//      AlarmManager，标误报/确认都要经这里回到单例。
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
