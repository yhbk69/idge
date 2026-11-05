// ============================================================================
// alarm_list_widget.cpp - 报警数据页实现（类别报警 / 电子围栏 / 检测明细）
// ============================================================================
//
// 【所属模块】src/ui/widgets。一页三张表共用同一套检索条与同一份分页状态，
//   差别只在 currentAlarmFilter/currentDetectionFilter 把这份状态翻给哪个 DAO。
// 【数据流】用户动作（切 Tab、改下拉、点翻页、点刷新）或 300ms 防抖到的新报警
//   → reloadCurrentTab()（先过 isVisible() 门）→ 按当前 Tab 派给三个 refresh*Table
//   之一 → countByFilter（定总页数）→ clampPageToTotal() → queryByFilter（取当前页）
//   → 填表 + 缓存 pageAlarms_/pageFenceAlarms_/pageDetections_。
//   详情入口读的是这份缓存，**不再回查库、也不查内存队列**
//   （内存队列只有最近 1000 条，第 3 页以后的行在那里根本找不到）。
// 【为什么全在 GUI 线程】见头文件【线程/耗时】那一段：一页一屏时 7~35ms，
//   真正的代价是"别在不可见时干这件事"，所以 isVisible() 门比线程化更值。
// 【四条硬口径】① count→clamp→取行 的顺序；② 不可见 ⇒ 零 SQL；
//   ③ 新报警不翻动用户正在读的页（refreshForNewData 在 page_>0 时直接返回）；
//   ④ 没有任何文字输入框，取值一律点选。逐条实测数据见同目录 README。
// ============================================================================
#include "alarm_list_widget.h"
#include "perf_probe.h"
#include <QElapsedTimer>
#include <QDebug>
#include "runtime_paths.h"
#include "alarm_manager.h"
#include "construction_session.h"   // ConstructionInfo::display()：空值统一成 "—" 的唯一口径
#include "qthelper.h"
#include "alarm_detail_dialog.h"
#include "fence_manager.h"
#include "theme.h"
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
#include <QComboBox>
#include <QAbstractItemView>
#include <QCursor>
#include <QAction>

namespace {

// 一页的行数不写死，见 AlarmListWidget::pageSize()：含义是"当前那张表一屏能显示几行"。
// 以前的 200（更早是 500，含义还是"总共只画这么多"）在 56px 行高下要连滑二十多屏才够一页，
// 那不叫分页。留这个常量只作为视口还没布局出来时的兜底。
constexpr int kPageSizeFallback = 10;
// 上下限是给"视口高度被算歪"兜底的：1920x1200 上表格区一屏实测 11 行（行高 56），
// 5 是"再小也没法读"的下界（比它小通常是 viewport 还没布局出来），
// 50 是上限——超过它就等于回到 200 行/页那次的性能妥协（一次重建 300ms+）。
// 这两个数只钳 pageSize() 的算式结果，不参与任何业务判据。
constexpr int kPageSizeMin = 5;
constexpr int kPageSizeMax = 50;

enum TabIndex { TabClass = 0, TabFence = 1, TabDetection = 2 };

// 状态码 → 界面中文。库里是 pending/rectified/disposed/false_alarm 四种，
// 列表以前把非 rectified 一律显示"未确认"，误报于是和未确认撞成同一个词。
QString statusLabel(const QString &status)
{
    if (status == QLatin1String("rectified")) return QStringLiteral("已确认");
    if (status == QLatin1String("disposed")) return QStringLiteral("已处置");
    if (status == QLatin1String("false_alarm")) return QStringLiteral("误报");
    return QStringLiteral("未确认");
}

// 状态色。三个 RGB 是 theme 里那三个语义色的**同一组 Material 取值**
// （SUCCESS #4caf50 / 中性灰 #9E9E9E / WARNING #ff9800），这里必须用 QColor 而不是
// theme:: 的字符串常量—— QTableWidgetItem::setForeground 要的是 QColor。
// 灰是给"误报"的：它不再是待处理项，但又不该看起来像错误，所以不占红也不占橙。
// 蓝是给"已处置"的：它和"已确认"（绿）在板端是两件事——一个是走完了处置表单，
// 一个只是有人看过，撞成同一个颜色就等于把闭环进度读成已读进度。取值同 theme::INFO。
QColor statusColor(const QString &status)
{
    if (status == QLatin1String("rectified")) return QColor(76, 175, 80);    // 绿
    if (status == QLatin1String("disposed")) return QColor(33, 150, 243);    // 蓝（theme::INFO）
    if (status == QLatin1String("false_alarm")) return QColor(158, 158, 158); // 灰
    return QColor(255, 152, 0);                                              // 橙
}

// 触屏档：这一页的检索条是"主要操作"，控件一律给到能按准的尺寸。
// 表头/Tab 的内联 QSS 特异性高于 blacksoft.css，皮肤层放大的 padding 会被打回原形，
// 所以下拉本体的高度必须在这里一起给。
// ⚠ 弹出来的那一层**不在这条 QSS 的管辖范围内**：`QComboBox QAbstractItemView::item
//    { min-height }` 无论写在 combo 上还是写在 view() 上都不生效（2026-10-31 独立探针实测：
//    两种写法弹层行高都停在字体算出的尺寸），行高只能由 item delegate 给 —— 见 makeCombo。
QString comboQss()
{
    return QStringLiteral(
        "QComboBox { background: %1; color: %2; border: 1px solid %3; border-radius: 4px;"
        "  min-height: %4px; padding: 6px 10px; }"
        "QComboBox::drop-down { width: 30px; }"
        "QComboBox QAbstractItemView { background: %1; color: %2; border: 1px solid %3;"
        "  selection-background-color: %5; }"
        "QComboBox QAbstractItemView::item { min-height: %7px; }")
        .arg(theme::FIELD, theme::TEXT, theme::BORDER)
        .arg(theme::TOUCH_FIELD_MIN_H)
        .arg(theme::BTN_SECONDARY)
        .arg(theme::TOUCH_FIELD_MIN_H);
}

// 把候选值灌进下拉，并按 data 找回原来的选择。
// blockSignals 是必需的：灌值会触发 currentIndexChanged → onFilterChanged → 重查库 →
// 而 rebuildFacets 正是被这次条件变化叫起来的，于是无限递归。
void fillCombo(QComboBox *combo, const QString &allText,
               const QVector<QPair<QString, int>> &facets)
{
    const QVariant wanted = combo->currentData();
    combo->blockSignals(true);
    combo->clear();
    combo->addItem(allText, QString());
    for (const auto &f : facets) {
        // 带上条数：用户点之前就知道这一项有多少东西，比点了才知道空强。
        combo->addItem(QStringLiteral("%1 (%2)").arg(f.first).arg(f.second), f.first);
    }

    int target = 0;
    if (!wanted.toString().isEmpty()) {
        const int found = combo->findData(wanted);
        // 找回不到就退回"全部"：留着选一个当前候选里没有的值，
        // 结果是零行而原因藏在看不见的那行小字里，这是自找的困惑。
        target = (found >= 0) ? found : 0;
    }
    combo->setCurrentIndex(target);
    combo->blockSignals(false);
}

// 读下拉的当前值，空指针与"还没有项"都归一成空 QVariant（= 该维度不限）。
// 判空不是走形式：构造期与切 Tab 之前，combo 可能还是 nullptr（setupFilterBar 之后
// 才有的件），直接 currentData() 就是空指针解引用；而 currentIndex() 为 -1
// （刚 clear 还没灌值那一瞬）时 Qt 给的也是无效 QVariant，这里一并收成空值。
// 调用方（两个 currentXxxFilter）依赖的正是"空串 = 该字段不限"这一条统一口径。
QVariant comboData(const QComboBox *combo)
{
    return (combo && combo->currentIndex() >= 0) ? combo->currentData() : QVariant();
}

} // namespace

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

    // 首屏数据在 showEvent 里拉：构造期数据库不一定已经打开。
}

/*
====================================================
作用：防抖刷新槽函数
说明：执行实际的表格刷新操作
====================================================
*/
void AlarmListWidget::onDebouncedRefresh()
{
    QElapsedTimer probe; probe.start();
    refreshForNewData();
    if (perfProbe() && probe.elapsed() > 5) {
        qWarning() << "[PROBE] alarmList refreshTable" << probe.elapsed() << "ms";
    }
}

void AlarmListWidget::refreshForNewData()
{
    // 见声明处：用户在读第 2 页往后的内容时，新报警不许把他的行换掉。
    if (page_ > 0) return;
    reloadCurrentTab();
}

/*
====================================================
作用：统计更新槽函数
说明：只做一件事 —— 让**可见时**的页面在 300ms 内重查一次。
      这里刻意一个数字都不写：这一页所有计数现在都以库为准，而查库只能发生在
      reloadCurrentTab() 那一次防抖里。若在这里也写一遍（还是内存队列那 1000 条的口径），
      "未确认"与"命中"就会各说各话 —— 一个是最近 1000 条里的未确认，一个是全部历史里的命中。
====================================================
*/
void AlarmListWidget::onStatsUpdated()
{
    if (isVisible()) refreshTimer_->start();
}

/*
====================================================
作用：页面显示事件处理
说明：页面重新可见时刷新一次数据，并在此第一次拉取筛选候选值
说明：因为不可见期间只更新了统计数字，需要完整刷新表格
====================================================
*/
void AlarmListWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (!facetsReady_) {
        facetsReady_ = true;
        rebuildFacets();     // 依赖数据库已打开，所以放在首次可见时做
    }
    reloadCurrentTab();      // 完整刷新当前 Tab 那张表
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
    header->setStyleSheet(theme::text(theme::TEXT, theme::FS_PAGE, true));
    mainLay->addWidget(header);

    /*
    ====================================================
    统计信息区域
    说明：显示当前检索条件的命中数与未确认数
    ====================================================
    */
    QHBoxLayout *statsLay = new QHBoxLayout();
    lblTotal_ = new QLabel("共 0 条");  // 命中数标签（当前条件）
    lblTotal_->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
    statsLay->addWidget(lblTotal_);

    lblUnack_ = new QLabel("未确认: 0 条");  // 未确认数标签
    lblUnack_->setStyleSheet(theme::text(theme::WARNING, theme::FS_BODY));
    statsLay->addWidget(lblUnack_);

    statsLay->addStretch();  // 弹性空间
    mainLay->addLayout(statsLay);

    setupFilterBar(mainLay);

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
    btnRefresh->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                                  .arg(theme::TEXT, theme::BTN_SECONDARY));
    connect(btnRefresh, &QPushButton::clicked, this, &AlarmListWidget::onRefresh);
    toolLay->addWidget(btnRefresh);

    // 全部确认按钮
    QPushButton *btnAck = new QPushButton("全部确认");
    btnAck->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                              .arg(theme::TEXT, theme::SUCCESS));
    connect(btnAck, &QPushButton::clicked, this, &AlarmListWidget::onAcknowledge);
    toolLay->addWidget(btnAck);

    // 清空按钮
    QPushButton *btnClear = new QPushButton("清空");
    btnClear->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                                .arg(theme::TEXT, theme::DANGER));
    connect(btnClear, &QPushButton::clicked, this, &AlarmListWidget::onClear);
    toolLay->addWidget(btnClear);

    // 打开截图目录按钮
    QPushButton *btnOpenDir = new QPushButton("打开截图目录");
    btnOpenDir->setStyleSheet(QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                                  .arg(theme::TEXT, theme::WARNING));
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
        ? QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")  // 绿色（开启）
              .arg(theme::TEXT, theme::SUCCESS)
        : QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")  // 灰色（关闭）
              .arg(theme::TEXT_MUTED, theme::HOVER));

    // 截图开关切换处理
    connect(snapBtn_, &QPushButton::toggled, this, [this](bool on) {
        AlarmManager::instance().setScreenshotsEnabled(on);  // 保存设置
        snapBtn_->setText(on ? "截图: 开" : "截图: 关");
        snapBtn_->setStyleSheet(on
            ? QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                  .arg(theme::TEXT, theme::SUCCESS)
            : QString("color: %1; background: %2; border-radius: 4px; padding: 6px 16px;")
                  .arg(theme::TEXT_MUTED, theme::HOVER));
    });
    toolLay->addWidget(snapBtn_);

    mainLay->addLayout(toolLay);

    /*
    ====================================================
    Tab 切换（类别报警 / 电子围栏 / 检测明细）
    ====================================================
    */
    tabWidget_ = new QTabWidget(this);
    // 内联 QSS 特异性高于 blacksoft.css，Tab/表头的触屏档必须在这里一起给，
    // 否则皮肤层放大的 padding 会被这两条打回原形。
    tabWidget_->setStyleSheet(
        QString("QTabWidget::pane { border: 1px solid %1; background: %2; }"
                "QTabBar::tab { background: %3; color: %4; padding: 16px 24px;"
                "  border: 1px solid %1; border-bottom: none; }"
                "QTabBar::tab:selected { background: %2; color: %5; }")
            .arg(theme::BORDER, theme::BG, theme::PANEL, theme::TEXT_MUTED, theme::TEXT));

    const QString tableQss =
        QString("QTableWidget { background: %1; color: %2; gridline-color: %3; }"
                "QTableWidget::item:selected { background: %4; }"
                "QHeaderView::section { background: %5; color: %6; padding: 10px 12px;"
                "  border: 1px solid %3; }")
            .arg(theme::BG, theme::TEXT, theme::BORDER, theme::BTN_SECONDARY, theme::PANEL,
                 theme::TEXT_MUTED);

    /*
    ====================================================
    告警表格（类别报警 Tab）
    ====================================================
    */
    table_ = new QTableWidget();
    // 1920 宽下再加就必截字，所以列表**只这一列**负责人，其余施工信息进详情框。
    // uuid 那一列已撤：界面上除了它没有第二处能读到报警 id（详情框里也不显示了），
    // 而它是排查时唯一能被抄进聊天框的一串字符 —— 真要定位某一条，用时间+通道+类别。
    table_->setColumnCount(7);
    table_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "负责人", "状态", "操作"});
    // 撤掉最后一列后「操作」变成了最后一列：stretchLastSection 会无视它的 resize mode
    // 把详情钮拉成一条几百像素的白条（围栏表同一个坑，见下面 fenceTable_ 的写法）。
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    table_->setColumnWidth(6, 100);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setAlternatingRowColors(true);
    table_->setStyleSheet(tableQss);
    table_->verticalHeader()->setVisible(false);
    // 表格行高不吃 CSS 的 ::item padding（那是 item delegate 的内边距，不改 section 高度），
    // 只能在这里设 default section size —— 56px ≈ 6.3mm，指肚能按准一行。
    table_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    connect(table_, &QTableWidget::cellDoubleClicked,
            this, &AlarmListWidget::onRowDoubleClicked);
    // 表头筛选这条连线以前不存在：onHeaderSectionClicked 整个函数是死代码，
    // 于是"通道/类别可筛选"只写在注释里，点列头永远没反应。
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &AlarmListWidget::onHeaderSectionClicked);
    tabWidget_->addTab(table_, "类别报警");

    /*
    ====================================================
    围栏报警表格（电子围栏 Tab）
    ====================================================
    */
    fenceTable_ = new QTableWidget();
    fenceTable_->setColumnCount(7);
    fenceTable_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "负责人", "围栏类型", "操作"});
    // 原来靠 setStretchLastSection(true) 把「围栏类型」撑满；加了「操作」列后它就是最后一列，
    // 而 stretchLastSection 会**无视**该列的 resize mode，把按钮拉成 300px 宽的白条。
    // 所以改成显式指定：时间/围栏类型两列分剩余宽度（与改动前等价），操作列定宽。
    fenceTable_->horizontalHeader()->setStretchLastSection(false);
    fenceTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    fenceTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    fenceTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Fixed);
    fenceTable_->setColumnWidth(6, 100);
    fenceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fenceTable_->setAlternatingRowColors(true);
    fenceTable_->setStyleSheet(tableQss);
    fenceTable_->verticalHeader()->setVisible(false);
    fenceTable_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    // 双击入口保留：改手势语义（双击→单击）是产品决策，本轮只做**加法**。
    // 但触屏上双击不可靠是事实，所以另加一颗「详情」钮作为可点的入口（见 refreshFenceTable）。
    connect(fenceTable_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) {
                // 围栏行也能看详情：以前只有类别表接了双击，围栏报警点开无门。
                if (row >= 0 && row < pageFenceAlarms_.size()) {
                    showAlarmDetail(pageFenceAlarms_.at(row));
                }
            });
    connect(fenceTable_->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &AlarmListWidget::onHeaderSectionClicked);
    tabWidget_->addTab(fenceTable_, "电子围栏");

    /*
    ====================================================
    检测明细表格（检测明细 Tab）
    说明：detections 表以前是**只写不读**的 —— DetectionDAO 的查询接口全仓零调用，
          配置项 database.storeDetections 打开后落了几万行，没有任何界面能看到它们。
    ====================================================
    */
    detTable_ = new QTableWidget();
    detTable_->setColumnCount(5);
    detTable_->setHorizontalHeaderLabels({"时间", "通道", "类别", "置信度", "检测框"});
    detTable_->horizontalHeader()->setStretchLastSection(true);
    detTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    detTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    detTable_->setAlternatingRowColors(true);
    detTable_->setStyleSheet(tableQss);
    detTable_->verticalHeader()->setVisible(false);
    detTable_->verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW);
    tabWidget_->addTab(detTable_, "检测明细");

    connect(tabWidget_, &QTabWidget::currentChanged, this, &AlarmListWidget::onTabChanged);

    mainLay->addWidget(tabWidget_, 1);
}

/*
====================================================
作用：检索条
说明：一行 —— 五个下拉 + 翻页 + 清除筛选。没有任何文字输入框：本机不做虚拟键盘
      （见 src/ui/core/README.md 触屏口径），所以"检索"在这一页只能是点选，
      关键字框要等虚拟键盘落地才有意义。
      2026-11-02 撤掉了原先**上面那一行时间档位**（今天/近3天/近7天/全部）：这一页是
      围着施工项目看的，而"当前选的是哪一档"在表格里问不出来 —— 档位在屏幕外时，
      "今天确实没出报警"和"筛错了档"长得一模一样。现在恒为不限时间（startTime/endTime
      留 0，DAO 里就是不加时间条件），要收窄有通道/类别/状态/负责人/施工id 五个下拉。
====================================================
*/
void AlarmListWidget::setupFilterBar(QVBoxLayout *mainLay)
{
    QHBoxLayout *comboLay = new QHBoxLayout();
    comboLay->setSpacing(10);

    auto makeCombo = [this, comboLay](const QString &allText) {
        QComboBox *c = new QComboBox();
        c->setStyleSheet(comboQss());
        theme::makeComboTouch(c);
        c->addItem(allText, QString());
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this](int) { onFilterChanged(); });
        comboLay->addWidget(c);
        return c;
    };

    comboChannel_ = makeCombo("全部通道");
    // "全部"这一档的 data 必须是 -1 而不是 makeCombo 给的 QString()：
    // 空 QString 走 toInt() 得到 0，而 0 是**通道 1**，默认视图会莫名只出第一路。
    comboChannel_->setItemData(0, -1);
    for (int i = 0; i < 4; ++i) {
        comboChannel_->addItem(QString("通道 %1").arg(i + 1), i);   // 与表格"通道 N"同一 0 基口径
    }
    comboClass_ = makeCombo("全部类别");
    comboStatus_ = makeCombo("全部（不含误报）");
    comboStatus_->addItem("未确认", QStringLiteral("pending"));
    comboStatus_->addItem("已确认", QStringLiteral("rectified"));
    comboStatus_->addItem("已处置", QStringLiteral("disposed"));
    comboStatus_->addItem("误报", QStringLiteral("false_alarm"));
    comboWorker_ = makeCombo("全部负责人");
    comboConstruction_ = makeCombo("全部施工id");

    comboLay->addStretch();

    // 分页条：首页 · 上一页 · 第 N / M 页 ▾ · 下一页 · 末页。
    // 「第 N / M 页」必须是按钮：QLabel 收不到点击，而本机没有虚拟键盘，
    // 跳页就只能靠点这个数字弹一个可点选的菜单。
    btnFirstPage_ = new QPushButton("首页");
    btnPrevPage_ = new QPushButton("◀ 上一页");
    btnPage_ = new QPushButton("第 1 / 1 页 ▾");
    btnNextPage_ = new QPushButton("下一页 ▶");
    btnLastPage_ = new QPushButton("末页");

    const QString navQss =
        QString("QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 4px;"
                "  min-height: %4px; padding: 6px 14px; }"
                "QPushButton:disabled { color: %5; }")
            .arg(theme::TEXT, theme::FIELD, theme::BORDER)
            .arg(theme::TOUCH_FIELD_MIN_H)
            .arg(theme::TEXT_MUTED);
    // 页码那颗同高但透明底：一行里五个实心按钮会看不出哪个是页码，
    // 而它的角色是"跳转入口"，不是又一个翻页动作。
    const QString pageQss =
        QString("QPushButton { color: %1; background: transparent; border: 1px solid %2;"
                " border-radius: 4px; min-height: %3px; padding: 6px 14px; }")
            .arg(theme::TEXT_MUTED, theme::BORDER)
            .arg(theme::TOUCH_FIELD_MIN_H);

    for (QPushButton *b : {btnFirstPage_, btnPrevPage_, btnPage_, btnNextPage_, btnLastPage_}) {
        b->setStyleSheet(b == btnPage_ ? pageQss : navQss);
        comboLay->addWidget(b);
    }
    connect(btnFirstPage_, &QPushButton::clicked, this, [this]() { gotoPage(0); });
    connect(btnPrevPage_, &QPushButton::clicked, this, [this]() { onPageStep(-1); });
    connect(btnPage_, &QPushButton::clicked, this, &AlarmListWidget::openPageJumpMenu);
    connect(btnNextPage_, &QPushButton::clicked, this, [this]() { onPageStep(1); });
    connect(btnLastPage_, &QPushButton::clicked, this, [this]() { gotoPage(pageCount() - 1); });

    // 清除筛选排在翻页之后：它是这一行的"复位"，和翻页不是一类动作，
    // 混在左侧那五个下拉中间会被当成第六个条件。
    btnClearFilter_ = new QPushButton("清除筛选");
    btnClearFilter_->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border: 1px solid %3; border-radius: 4px;"
                "  min-height: %4px; padding: 6px 14px; }")
            .arg(theme::TEXT, theme::BTN_SECONDARY, theme::BORDER)
            .arg(theme::TOUCH_MAIN_MIN_H));
    connect(btnClearFilter_, &QPushButton::clicked, this, [this]() {
        // 清除 = 五个下拉 + 围栏角色一起回到"全部"再回第一页。
        // 复位必须一次做完：按下之后按钮一个没动而数字变了，就是又造出一个看不见的条件。
        for (QComboBox *c : {comboChannel_, comboClass_, comboStatus_,
                             comboWorker_, comboConstruction_}) {
            if (c) { c->blockSignals(true); c->setCurrentIndex(0); c->blockSignals(false); }
        }
        fenceRole_ = AlarmFilter::RoleAll;
        page_ = 0;
        reloadCurrentTab();
    });
    comboLay->addWidget(btnClearFilter_);

    mainLay->addLayout(comboLay);
}

/*
====================================================
作用：当前筛选条件 → DAO 的查询条件
说明：时间两项恒为 0（= 不限）。2026-11-02 撤掉了时间档位，这里不再换算区间；
      DAO 侧的 startTime/endTime 参数留着，是把区间还给界面的现成口子。
====================================================
*/
AlarmFilter AlarmListWidget::currentAlarmFilter(AlarmFilter::Scope scope) const
{
    AlarmFilter f;
    f.channel = comboChannel_ ? comboChannel_->currentData().toInt() : -1;
    f.className = comboData(comboClass_).toString();
    f.status = comboData(comboStatus_).toString();
    f.workerName = comboData(comboWorker_).toString();
    f.taskId = comboData(comboConstruction_).toString();
    f.fenceRole = fenceRole_;
    f.scope = scope;
    f.limit = pageSize();
    f.offset = page_ * f.limit;   // 偏移由 limit 推，别和上面那个数各算一遍
    return f;
}

DetectionFilter AlarmListWidget::currentDetectionFilter() const
{
    DetectionFilter f;
    f.channel = comboChannel_ ? comboChannel_->currentData().toInt() : -1;
    f.className = comboData(comboClass_).toString();
    f.limit = pageSize();
    f.offset = page_ * f.limit;   // 偏移由 limit 推，别和上面那个数各算一遍
    return f;
}

/*
====================================================
作用：重建筛选下拉的候选值
说明：候选值与条数按**全量**（不限时间）统计，不跟随其它已选条件（见 alarm_dao.h 的
      facets 说明）。负责人/施工id 只在报警两张表有意义，检测明细 Tab 不重新拉。
====================================================
*/
void AlarmListWidget::rebuildFacets()
{
    AlarmDAO dao;
    const long start = 0, end = 0;   // 0/0 = 不限时间（时间档位已撤，见 setupFilterBar）

    const bool fenceTab = (tabWidget_ && tabWidget_->currentIndex() == TabFence);
    const AlarmFilter::Scope scope =
        fenceTab ? AlarmFilter::FenceOnly : AlarmFilter::ClassOnly;

    fillCombo(comboClass_, QStringLiteral("全部类别"), dao.classFacets(start, end, scope));

    if (fenceTab || (tabWidget_ && tabWidget_->currentIndex() == TabClass)) {
        fillCombo(comboWorker_, QStringLiteral("全部负责人"),
                  dao.workerFacets(start, end, scope));
        fillCombo(comboConstruction_, QStringLiteral("全部施工id"),
                  dao.taskIdFacets(start, end, scope));
    }
}

/*
====================================================
作用：只重建当前可见的那张表
说明：三张表同时查是三次代价而屏幕同时只放得下一张；页面隐藏时一律不查，
      切回来由 showEvent 补 —— 这是这一轮唯一的性能纪律，别再破它。
====================================================
*/
void AlarmListWidget::reloadCurrentTab()
{
    if (!isVisible()) return;   // 页面不在眼前就一次库都不查，切回来由 showEvent 补

    switch (tabWidget_ ? tabWidget_->currentIndex() : TabClass) {
    case TabFence:      refreshFenceTable();     break;
    case TabDetection:  refreshDetectionTable(); break;
    default:            refreshTable();          break;
    }

    // 未确认数也从库里取（覆盖索引计数，实测 27858 条 pending 上 11ms），
    // 和表格同一次刷新里出，两个数字因此永远同时点。
    const int unack = AlarmDAO().unacknowledgedCount();
    lblUnack_->setText(QStringLiteral("未确认: %1 条").arg(unack));
    lblUnack_->setStyleSheet(unack > 0
        ? theme::text(theme::WARNING, theme::FS_BODY)
        : theme::text(theme::SUCCESS, theme::FS_BODY));
}

/*
====================================================
作用：一页多少行 = 当前那张表一屏能显示几行
说明：见文件头 kPageSizeFallback 的注释。取 viewport 高度而不是 widget 高度：
      表头不占行。少算 1px 是为了不把"露出一点点的半行"也算进一屏。
====================================================
*/
int AlarmListWidget::pageSize() const
{
    QTableWidget *t = table_;
    if (tabWidget_) {
        const int idx = tabWidget_->currentIndex();
        if (idx == TabFence && fenceTable_) t = fenceTable_;
        else if (idx == TabDetection && detTable_) t = detTable_;
    }
    if (!t) return kPageSizeFallback;

    const int rowH = qMax(1, t->verticalHeader()->defaultSectionSize());
    const int viewH = t->viewport()->height();
    if (viewH <= 0) return kPageSizeFallback;   // 还没布局出来（首次刷新前不该发生，兜底而已）
    return qBound(kPageSizeMin, (viewH - 1) / rowH, kPageSizeMax);
}

// 一共几页：**恒 ≥1**（0 条结果时是"第 1 / 1 页"而不是"第 1 / 0 页"）。
// totalRows_ 还没被任何一次 countByFilter 写过时是 0，也走这条，
// 所以调用方可以在首屏刷新前安全地拿它做 qBound 的上界。
int AlarmListWidget::pageCount() const
{
    const int per = qMax(1, pageSize());
    return qMax(1, (totalRows_ + per - 1) / per);
}

// 把当前页钳回合法区间。+1 页减 1 页都当成上界，是因为 pageCount() 至少给 1。
// 调用点只有一处是"必须的"：三个 refresh*Table 里 countByFilter 之后、取行之前
// （条件变严使总页数缩小时，page_ 留着旧值就是拿一个不存在的 offset 去查库）。
// 翻页那两条（onPageStep/gotoPage）自己已经 qBound 过，这里是第二道保险。
void AlarmListWidget::clampPageToTotal()
{
    page_ = qBound(0, page_, pageCount() - 1);
}

/*
====================================================
作用：更新命中数、分页标签与翻页按钮可用态
====================================================
*/
void AlarmListWidget::updatePagingLabel()
{
    const QString status = comboData(comboStatus_).toString();
    lblTotal_->setText(status.isEmpty()
        ? QStringLiteral("命中 %1 条（不含误报）").arg(totalRows_)
        : QStringLiteral("命中 %1 条（%2）").arg(totalRows_).arg(statusLabel(status)));

    const int pages = pageCount();
    // ▾ 是"这颗能点"的唯一提示：跳页菜单里给首页/末页、附近页码与 ±10/±100，
    // 本轮没有虚拟键盘，所以不能给一个"输入页码"的框。
    btnPage_->setText(QStringLiteral("第 %1 / %2 页 ▾").arg(page_ + 1).arg(pages));

    btnFirstPage_->setEnabled(page_ > 0);
    btnPrevPage_->setEnabled(page_ > 0);
    btnNextPage_->setEnabled(page_ < pages - 1);
    btnLastPage_->setEnabled(page_ < pages - 1);
}

/*
====================================================
作用：刷新表格数据
说明：查数据库（当前条件 + 当前页），填类别报警表
====================================================
*/
void AlarmListWidget::refreshTable()
{
    if (!isVisible()) return;   // 不可见时不查库也不重建，见 reloadCurrentTab 的同一道门

    AlarmDAO dao;
    AlarmFilter filter = currentAlarmFilter(AlarmFilter::ClassOnly);
    totalRows_ = dao.countByFilter(filter);
    clampPageToTotal();          // 先钳页再取行：反过来的话"第 3 / 2 页"这种自相矛盾真会出现
    filter.offset = page_ * filter.limit;
    pageAlarms_ = dao.queryByFilter(filter);
    updatePagingLabel();

    const int shown = pageAlarms_.size();
    table_->setRowCount(shown);
    for (int i = 0; i < shown; ++i) {
        const AlarmRecord &a = pageAlarms_.at(i);

        // 时间列
        QDateTime dt = QDateTime::fromString(a.alarmTime, Qt::ISODate);
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        timeItem->setData(Qt::UserRole, a.imagePath);  // 存储截图路径
        timeItem->setData(Qt::UserRole + 2, a.id);     // 存储报警ID（用于精确定位）
        table_->setItem(i, 0, timeItem);

        // 通道列
        table_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(a.channel + 1)));

        // 类别列
        table_->setItem(i, 2, new QTableWidgetItem(a.className));

        // 置信度列
        QTableWidgetItem *confItem = new QTableWidgetItem(QString("%1%").arg(a.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);  // 右对齐
        table_->setItem(i, 3, confItem);

        // 负责人列：施工信息里唯一进列表的一项（1920 宽再加就截字）。
        // 未登录/历史行为空 → 统一显示 "—"，与详情框、卡片、toast 同一个口径。
        table_->setItem(i, 4, new QTableWidgetItem(
            session::ConstructionInfo::display(a.workerName)));

        // 状态列
        QTableWidgetItem *statusItem = new QTableWidgetItem(statusLabel(a.status));
        statusItem->setForeground(statusColor(a.status));
        table_->setItem(i, 5, statusItem);

        // 操作列（详情按钮）：触屏档 T3。这条是报警详情的**唯一可靠入口** ——
        // 表格双击在触屏上不可靠，但那是手势问题，本轮不动。
        QPushButton *btnDetail = new QPushButton("详情");
        btnDetail->setStyleSheet(
            QString("QPushButton { color: %1; background: %2; border-radius: 4px;"
                    "  min-height:%4px; padding: 8px 14px; }"
                    "QPushButton:hover { background: %3; }")
                .arg(theme::TEXT, theme::BTN_SECONDARY, theme::BTN_SECONDARY_HOVER)
                .arg(theme::TOUCH_ROW_MIN_H));
        connect(btnDetail, &QPushButton::clicked, this, [this, i]() {
            if (i < pageAlarms_.size()) showAlarmDetail(pageAlarms_.at(i));
        });
        table_->setCellWidget(i, 6, btnDetail);
    }
}

/*
====================================================
作用：刷新围栏报警表格
====================================================
*/
void AlarmListWidget::refreshFenceTable()
{
    // 与 refreshTable 同一道可见性门控：本页隐藏时不重建、不查库。
    if (!isVisible()) return;

    AlarmDAO dao;
    AlarmFilter filter = currentAlarmFilter(AlarmFilter::FenceOnly);
    totalRows_ = dao.countByFilter(filter);
    clampPageToTotal();
    filter.offset = page_ * filter.limit;
    pageFenceAlarms_ = dao.queryByFilter(filter);
    updatePagingLabel();

    const int shown = pageFenceAlarms_.size();
    fenceTable_->setRowCount(shown);
    for (int i = 0; i < shown; ++i) {
        const AlarmRecord &a = pageFenceAlarms_.at(i);

        QDateTime dt = QDateTime::fromString(a.alarmTime, Qt::ISODate);
        QTableWidgetItem *timeItem = new QTableWidgetItem(dt.toString("yyyy-MM-dd HH:mm:ss"));
        timeItem->setData(Qt::UserRole, a.imagePath);
        timeItem->setData(Qt::UserRole + 2, a.id);
        fenceTable_->setItem(i, 0, timeItem);

        fenceTable_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(a.channel + 1)));
        fenceTable_->setItem(i, 2, new QTableWidgetItem(a.className));

        QTableWidgetItem *confItem = new QTableWidgetItem(QString("%1%").arg(a.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        fenceTable_->setItem(i, 3, confItem);

        // 负责人列（与类别报警表同一列位、同一 "—" 口径）
        fenceTable_->setItem(i, 4, new QTableWidgetItem(
            session::ConstructionInfo::display(a.workerName)));

        // 第 5 列出角色：一条通道可以同时有禁区和岗区域，都写"围栏"就看不出
        // 报的是"闯进来了"还是"人跑出去了"。老库里的 "fence" 归到闯入。
        fenceTable_->setItem(i, 5, new QTableWidgetItem(
                a.alarmType == QLatin1String("fence_out")
                    ? QStringLiteral("离岗")
                    : QStringLiteral("闯入禁区")));

        // 操作列：围栏 Tab 原本行内**零**可点件，详情的唯一入口是 cellDoubleClicked，
        // 而触屏上双击不可靠（两次按下之间极易被识别成拖动/滚动）。与类别报警表同档 T3。
        QPushButton *btnDetail = new QPushButton(QStringLiteral("详情"));
        btnDetail->setStyleSheet(theme::miniButton(theme::BTN_SECONDARY));
        connect(btnDetail, &QPushButton::clicked, this, [this, i]() {
            if (i < pageFenceAlarms_.size()) showAlarmDetail(pageFenceAlarms_.at(i));
        });
        fenceTable_->setCellWidget(i, 6, btnDetail);
    }
}

/*
====================================================
作用：刷新检测明细表格
说明：时间带毫秒 —— 同一秒内落多条是常态（实测同一秒 3 条），只到秒的话
      相邻几行看起来一模一样，没法分辨。
====================================================
*/
void AlarmListWidget::refreshDetectionTable()
{
    if (!isVisible()) return;

    DetectionDAO dao;
    DetectionFilter filter = currentDetectionFilter();
    totalRows_ = dao.countByFilter(filter);
    clampPageToTotal();
    filter.offset = page_ * filter.limit;
    pageDetections_ = dao.queryByFilter(filter);
    updatePagingLabel();

    const int shown = pageDetections_.size();
    detTable_->setRowCount(shown);
    for (int i = 0; i < shown; ++i) {
        const DetectionRecord &d = pageDetections_.at(i);

        detTable_->setItem(i, 0, new QTableWidgetItem(
            QDateTime::fromMSecsSinceEpoch(d.timestamp).toString("yyyy-MM-dd HH:mm:ss.zzz")));
        detTable_->setItem(i, 1, new QTableWidgetItem(QString("通道 %1").arg(d.channel + 1)));
        detTable_->setItem(i, 2, new QTableWidgetItem(d.className));

        QTableWidgetItem *confItem =
            new QTableWidgetItem(QString("%1%").arg(d.confidence * 100, 0, 'f', 1));
        confItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        detTable_->setItem(i, 3, confItem);

        detTable_->setItem(i, 4, new QTableWidgetItem(
            QStringLiteral("(%1,%2)-(%3,%4)")
                .arg(d.bboxLeft).arg(d.bboxTop).arg(d.bboxRight).arg(d.bboxBottom)));
    }
}

/*
====================================================
作用：手动刷新槽函数
说明：响应用户点击刷新按钮 —— 保留条件与页码，重查一次
====================================================
*/
void AlarmListWidget::onRefresh()
{
    reloadCurrentTab();
}

/*
====================================================
作用：确认告警槽函数
说明：现在「全部确认」是真的全部：库里 27858 条 pending 一次性 UPDATE，
      而不是遍历内存那 1000 条逐条改。因为动作范围跨几万行，先给确认框并报出条数。
====================================================
*/
void AlarmListWidget::onAcknowledge()
{
    // 条数取库不取内存队列：这个按钮现在改的是库里全部未确认，
    // 确认框报出的数字必须是它将要影响的行数，不能是内存那 1000 条里的零头。
    const int unack = AlarmDAO().unacknowledgedCount();
    if (QtHelper::askConfirm(this, QStringLiteral("全部确认"),
                             QStringLiteral("当前未确认 %1 条，全部标记为已确认？").arg(unack))
        != QMessageBox::Ok) {
        return;
    }
    AlarmManager::instance().acknowledgeAll();  // 一条 UPDATE 收掉库里全部未确认
    rebuildFacets();      // 候选值里的条数随之变化
    reloadCurrentTab();   // 刷新表格
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
    if (QtHelper::askConfirm(this, QStringLiteral("清空报警记录"),
                             QStringLiteral("确定要清空所有报警记录吗？"))
        == QMessageBox::Ok) {
        AlarmManager::instance().clearAlarms();  // 清空告警
        rebuildFacets();                         // 候选值随之消失
        reloadCurrentTab();  // 刷新表格
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
    QDir dir(RuntimePaths::alarmsDir());
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
    if (row >= 0 && row < pageAlarms_.size()) {
        showAlarmDetail(pageAlarms_.at(row));
    }
}

/*
====================================================
作用：显示告警详情Dialog
说明：记录直接来自当前页缓存（pageAlarms_/pageFenceAlarms_）。
      以前是拿 id 去 AlarmManager 的内存队列里找 —— 队列只有最近 1000 条，
      列表现在能翻到几万条之外，那条路径对老报警必然返回"找不到"然后什么都不发生。
====================================================
*/
void AlarmListWidget::showAlarmDetail(const AlarmRecord &alarm)
{
    AlarmDetailDialog dlg(alarm, this);
    connect(&dlg, &AlarmDetailDialog::alarmMarkedFalsePositive, this, [this](const QString &id) {
        AlarmManager::instance().markAsFalsePositiveById(id);
        reloadCurrentTab();
    });
    connect(&dlg, &AlarmDetailDialog::alarmAcknowledged, this, [this](const QString &id) {
        AlarmManager::instance().acknowledgeAlarmById(id);
        reloadCurrentTab();
    });
    dlg.exec();
}

/*
====================================================
作用：表格标题行点击槽函数
说明：点列头 = 打开该列的筛选菜单，选完写回检索条上的对应下拉
参数：logicalIndex - 列号（含义由当前 Tab 决定）
====================================================
*/
/**
 * @brief 表格标题行点击槽函数（弹出筛选菜单）
 *
 * 支持的筛选列（类别报警表）：
 *   - 列 1（通道）、列 2（类别）、列 4（负责人）、列 5（状态）
 * 支持的筛选列（电子围栏表）：
 *   - 列 1、列 2、列 4，外加列 5（围栏类型：闯入禁区 / 离岗）
 *
 * 菜单只改下拉，不另存一份条件：条件只有检索条那一套，表头是它的第二个入口。
 */
void AlarmListWidget::onHeaderSectionClicked(int logicalIndex)
{
    const bool fenceTab = (tabWidget_ && tabWidget_->currentIndex() == TabFence);

    QComboBox *target = nullptr;
    switch (logicalIndex) {
    case 1: target = comboChannel_; break;
    case 2: target = comboClass_; break;
    case 4: target = comboWorker_; break;
    case 5: target = fenceTab ? nullptr : comboStatus_; break;
    default: return;   // 时间/置信度/操作 这几列没有可筛的取值
    }

    // 围栏表的第 5 列是"围栏类型"，检索条上没有对应下拉，单独走角色菜单
    if (fenceTab && logicalIndex == 5) {
        QMenu menu(this);
        menu.setStyleSheet(
            QString("QMenu { background: %1; color: %2; border: 1px solid %3; }"
                    "QMenu::item { padding: 14px 24px; }"
                    "QMenu::item:selected { background: %4; }")
                .arg(theme::PANEL, theme::TEXT, theme::BORDER, theme::BTN_SECONDARY));
        menu.addAction(QStringLiteral("全部围栏类型"), this, [this]() {
            fenceRole_ = AlarmFilter::RoleAll; onFilterChanged(); });
        menu.addAction(QStringLiteral("闯入禁区"), this, [this]() {
            fenceRole_ = AlarmFilter::RoleIntrusion; onFilterChanged(); });
        menu.addAction(QStringLiteral("离岗"), this, [this]() {
            fenceRole_ = AlarmFilter::RoleLeave; onFilterChanged(); });
        menu.exec(QCursor::pos());
        return;
    }

    if (!target) return;

    QMenu menu(this);
    menu.setStyleSheet(
        QString("QMenu { background: %1; color: %2; border: 1px solid %3; }"
                "QMenu::item { padding: 14px 24px; }"
                "QMenu::item:selected { background: %4; }")
            .arg(theme::PANEL, theme::TEXT, theme::BORDER, theme::BTN_SECONDARY));

    // 菜单项直接来自那个下拉：候选值、计数、"全部"都在下拉里，这里不重算一遍。
    for (int i = 0; i < target->count(); ++i) {
        const QString text = target->itemText(i);
        const QVariant data = target->itemData(i);
        menu.addAction(text, this, [target, data]() {
            const int idx = target->findData(data);
            if (idx >= 0) target->setCurrentIndex(idx);   // 触发 onFilterChanged
        });
    }

    menu.exec(QCursor::pos());
}

/*
====================================================
作用：任一筛选条件变化
说明：条件变了必须回到第一页，否则会停在一个新结果集里不存在的页码上
====================================================
*/
void AlarmListWidget::onFilterChanged()
{
    page_ = 0;
    reloadCurrentTab();
}

/*
====================================================
作用：翻页
说明：钳到 pageCount() 而不是只钳下界 —— 跳转菜单能给 ±100，不钳上界会跑到
      一个不存在的页上（虽然 refresh 前 clampPageToTotal 会兜住，但按钮可用态
      是在那之后算的，中间会有一帧"下一页"该灰着却是亮的）。
====================================================
*/
void AlarmListWidget::onPageStep(int delta)
{
    page_ = qBound(0, page_ + delta, pageCount() - 1);
    reloadCurrentTab();
}

// 绝对跳转（跳页菜单用）。target 先钳进 [0, pageCount()-1] 再比，所以传 9999 也安全。
// "已经在这一页就直接返回"省的是**一整套** count+取行+重建（几十毫秒级），
// 跳页菜单里点当前页是很常见的动作，别把这行判等当多余优化删掉。
void AlarmListWidget::gotoPage(int page0)
{
    const int target = qBound(0, page0, pageCount() - 1);
    if (target == page_) return;   // 已经在这一页就别再查一遍库、再重建一遍表
    page_ = target;
    reloadCurrentTab();
}

/*
====================================================
作用：点「第 N / M 页」弹出跳页菜单
说明：全部可点选，没有输入框（本机不做虚拟键盘）。三段：
        首页/末页 —— 两端各一跳就到；
        ±10/±100  —— 粗调，目标页直接写在标签上，点的人不用自己算；
        附近页码  —— 当前页前后各 3 页，适合逐屏细看。
      菜单弹在按钮正下方而不是光标处：触屏没有光标。
====================================================
*/
void AlarmListWidget::openPageJumpMenu()
{
    const int pages = pageCount();

    QMenu menu(this);
    menu.setStyleSheet(
        QString("QMenu { background: %1; color: %2; border: 1px solid %3; }"
                "QMenu::item { padding: 14px 24px; }"
                "QMenu::item:selected { background: %4; }")
            .arg(theme::PANEL, theme::TEXT, theme::BORDER, theme::BTN_SECONDARY));

    QAction *toFirst = menu.addAction(QStringLiteral("首页（第 1 页）"));
    toFirst->setEnabled(page_ > 0);
    connect(toFirst, &QAction::triggered, this, [this]() { gotoPage(0); });

    QAction *toLast = menu.addAction(QStringLiteral("末页（第 %1 页）").arg(pages));
    toLast->setEnabled(page_ < pages - 1);
    connect(toLast, &QAction::triggered, this, [this, pages]() { gotoPage(pages - 1); });

    menu.addSeparator();

    // 粗调：目标页直接写在标签上，点的人不用自己算。
    // 落点等于当前页的项不给（贴边时 -100 与 -10 都会被钳到同一页）——
    // 一行点了没反应的菜单项不如没有，两行做同一件事更不如只留一行。
    int lastStepTarget = page_;
    for (const int delta : {-100, -10, 10, 100}) {
        const int target = qBound(0, page_ + delta, pages - 1);
        if (target == lastStepTarget) continue;
        lastStepTarget = target;
        QAction *act = menu.addAction(
            QString("%1 %2 页 → 第 %3 页")
                .arg(delta > 0 ? QStringLiteral("前进") : QStringLiteral("后退"))
                .arg(qAbs(delta))
                .arg(target + 1));
        connect(act, &QAction::triggered, this, [this, target]() { gotoPage(target); });
    }

    if (pages > 1) {
        menu.addSeparator();
        const int from = qMax(0, page_ - 3);
        const int to = qMin(pages - 1, page_ + 3);
        for (int p = from; p <= to; ++p) {
            if (p == page_) continue;
            QAction *act = menu.addAction(QStringLiteral("第 %1 页").arg(p + 1));
            connect(act, &QAction::triggered, this, [this, p]() { gotoPage(p); });
        }
    }

    menu.exec(btnPage_ ? btnPage_->mapToGlobal(QPoint(0, btnPage_->height()))
                       : QCursor::pos());
}

/*
====================================================
作用：Tab 切换
说明：检测明细表没有状态/负责人/施工id 这些列，切过去时把这三个下拉**置灰**，
      而不是留着让用户选了然后静默无效 —— 静默无效正是这一页以前的毛病。
====================================================
*/
void AlarmListWidget::onTabChanged()
{
    const bool detectionTab =
        (tabWidget_ && tabWidget_->currentIndex() == TabDetection);

    for (QComboBox *c : {comboStatus_, comboWorker_, comboConstruction_}) {
        if (!c) continue;
        c->setEnabled(!detectionTab);
        c->setToolTip(detectionTab
                          ? QStringLiteral("检测明细表没有这个字段")
                          : QString());
    }
    if (detectionTab) {
        // 这三个条件在检测明细里问不到，切过去就替用户清掉，避免"看不见的条件"限制着结果
        for (QComboBox *c : {comboStatus_, comboWorker_, comboConstruction_}) {
            if (c && c->currentIndex() != 0) {
                c->blockSignals(true);
                c->setCurrentIndex(0);
                c->blockSignals(false);
            }
        }
        DetectionDAO dao;
        const long start = 0, end = 0;   // 0/0 = 不限时间
        fillCombo(comboClass_, QStringLiteral("全部类别"), dao.classFacets(start, end));
    } else {
        rebuildFacets();
    }

    page_ = 0;
    reloadCurrentTab();
}
