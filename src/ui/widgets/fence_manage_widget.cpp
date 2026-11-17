// ============================================================================
// fence_manage_widget.cpp - 电子围栏管理页实现
// ============================================================================
// 页面只做两件事：把 PadFenceStore 的版本状态翻译成可用的按钮，把画布的
// 改动通过 save-draft/apply/disable/delete-draft 落库。状态机本身在 store 里，
// 这里不重复实现——各操作的顺序与判据与 pad_http_fences.cpp 的路由逐条对齐。
// ============================================================================

#include "fence_manage_widget.h"

#include "fence_canvas.h"
#include "construction_session.h"      // 当前施工会话：taskId / plan / owner 快照
#include "fence_manager.h"             // refreshFromStores
#include "fence_frame_renderer.h"      // apply 前渲染合成画面 / 草稿黑底预览
#include "qthelper.h"                  // askConfirm（中文确定/取消，左确认右取消）
#include "runtime_paths.h"             // 媒体根（alarmsDir）

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QVBoxLayout>

#include <utility>   // std::swap（Qt 5.12 的 QVector 没有 swap(i,j)）

using geofence::PadFence;
using geofence::PadFenceStore;
using geofence::PadFenceVersion;
using geofence::PadRegion;
using geofence::PadVersionStatus;
using geofence::FenceCanvas;
using geofence::CanvasMode;

namespace {

// 模式按钮 id → 画布模式（与 buildUi 里 addButton 的 id 一一对应）
CanvasMode modeFromId(int id)
{
    switch (id) {
    case 2: return CanvasMode::RectDraw;
    case 3: return CanvasMode::PolyDraw;
    case 4: return CanvasMode::DeleteMode;
    default: return CanvasMode::Edit;
    }
}

// save-draft 失败码 → 中文（与 pad_http_fences.cpp 的 outcome→信封映射同口径）
QString draftOutcomeText(PadFenceStore::DraftOutcome outcome, const QString &detail)
{
    switch (outcome) {
    case PadFenceStore::DraftOutcome::FenceNotFound:
        return QStringLiteral("本机没有这条围栏");
    case PadFenceStore::DraftOutcome::DraftAlreadyExists:
        return QStringLiteral("该摄像头已存在草稿，不能再新建");
    case PadFenceStore::DraftOutcome::NotDraftToEdit:
        return QStringLiteral("没有可编辑的草稿");
    case PadFenceStore::DraftOutcome::SourceVersionNotFound:
        return QStringLiteral("复制来源版本不存在");
    case PadFenceStore::DraftOutcome::CameraMismatch:
        return QStringLiteral("摄像头/任务与围栏不一致");
    case PadFenceStore::DraftOutcome::StoreError:
        return QStringLiteral("写入失败：%1").arg(detail);
    default:
        return QStringLiteral("未知错误");
    }
}

QString ruleDisplayName(const QString &ruleType)
{
    return ruleType == QLatin1String("CROSS") ? QStringLiteral("越界") : QStringLiteral("闯入");
}

QString statusDisplayName(PadVersionStatus status)
{
    switch (status) {
    case PadVersionStatus::Draft:    return QStringLiteral("草稿");
    case PadVersionStatus::Active:   return QStringLiteral("生效");
    case PadVersionStatus::Disabled: return QStringLiteral("已停用");
    }
    return QStringLiteral("未知");
}

// ISO 时间 → 界面可读（空 = 未发生，显示占位符而不是空串）
QString displayTime(const QString &iso)
{
    if (iso.isEmpty())
        return QStringLiteral("—");
    QString text = iso;
    text.replace(QLatin1Char('T'), QLatin1Char(' '));
    return text;
}

// 版本 → 底部提示（装载与点选版本共用，避免两处文案漂移）
QString versionStatusHint(const PadFenceVersion &version)
{
    switch (version.status) {
    case PadVersionStatus::Draft:
        return QStringLiteral("编辑草稿 v%1：画布改动点「保存草稿」落库，满意后「应用」")
            .arg(version.version);
    case PadVersionStatus::Active:
        return QStringLiteral("生效版本 v%1 只读：要改动请「另存为新方案」")
            .arg(version.version);
    case PadVersionStatus::Disabled:
        return QStringLiteral("已停用版本 v%1 只读：可「另存为新方案」再编辑")
            .arg(version.version);
    }
    return QString();
}

QString shapeDisplayName(const PadRegion &region)
{
    if (region.shapeType == QLatin1String("RECT"))
        return QStringLiteral("矩形");
    return QStringLiteral("多边形(%1点)").arg(region.points.size());
}

// 新区域的 regionId：与 FenceManager::generateRegionId 同格式（RG-<8hex>）。
// 那个函数是 FenceManager 的 private 内部件，这里按同一口径本地生成，
// 不去动 biz 头的可见性——Pad 围栏的 regionId 本就由"生成方"分配、原样保存。
QString newRegionId()
{
    const quint32 r = QRandomGenerator::global()->generate();
    return QStringLiteral("RG-%1").arg(r, 8, 16, QLatin1Char('0'));
}

}  // namespace

FenceManageWidget::FenceManageWidget(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    loadFenceForChannel();
}

void FenceManageWidget::setFrameGrabber(std::function<QImage(int channel)> grabber)
{
    frameGrabber_ = std::move(grabber);
}

void FenceManageWidget::refresh()
{
    loadFenceForChannel();
}

// ============================================================================
// 界面搭建
// ============================================================================

void FenceManageWidget::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    // 行 1：通道 + 名称 + 状态 + 全部生命周期操作（顶栏放满，画布尽量拿高度）
    auto *row1 = new QHBoxLayout;
    row1->addWidget(new QLabel(QStringLiteral("通道")));
    channelCombo_ = new QComboBox;
    // 显示用「通道N」，协议身份仍是 CHN（currentCameraId 按下标换算，不读显示文本）
    channelCombo_->addItems({QStringLiteral("通道1"), QStringLiteral("通道2"),
                             QStringLiteral("通道3"), QStringLiteral("通道4")});
    row1->addWidget(channelCombo_);
    row1->addSpacing(12);
    row1->addWidget(new QLabel(QStringLiteral("围栏名称")));
    nameEdit_ = new QLineEdit;
    nameEdit_->setMinimumWidth(320);   // 名称要显示全（原 220 上限会截断长名）
    row1->addWidget(nameEdit_);
    row1->addSpacing(12);
    stateLabel_ = new QLabel;
    row1->addWidget(stateLabel_);
    row1->addStretch(1);
    btnRefresh_ = new QPushButton(QStringLiteral("刷新"));
    btnNew_ = new QPushButton(QStringLiteral("新建围栏"));
    btnSave_ = new QPushButton(QStringLiteral("保存草稿"));
    btnApply_ = new QPushButton(QStringLiteral("应用"));
    btnCopy_ = new QPushButton(QStringLiteral("另存为新方案"));
    btnDelete_ = new QPushButton(QStringLiteral("删除草稿"));
    btnDisable_ = new QPushButton(QStringLiteral("停用"));
    for (QPushButton *btn : {btnRefresh_, btnNew_, btnSave_, btnApply_, btnCopy_, btnDelete_, btnDisable_}) {
        row1->addWidget(btn);
    }
    root->addLayout(row1);

    // 主区：左 = 编辑画布（拿全部剩余空间），右 = 版本信息 + 区域列表
    auto *mainLay = new QHBoxLayout;
    mainLay->setSpacing(8);

    auto *editorBox = new QGroupBox(QStringLiteral("围栏编辑"));
    auto *editorLay = new QVBoxLayout(editorBox);

    auto *toolLay = new QHBoxLayout;
    toolLay->addWidget(new QLabel(QStringLiteral("工具")));
    modeGroup_ = new QButtonGroup(this);
    modeGroup_->setExclusive(true);
    const QStringList modeTexts = {QStringLiteral("编辑"), QStringLiteral("画矩形"),
                                   QStringLiteral("画多边形"), QStringLiteral("删除")};
    for (int i = 0; i < modeTexts.size(); ++i) {
        auto *btn = new QPushButton(modeTexts.at(i));
        btn->setCheckable(true);
        modeGroup_->addButton(btn, i + 1);   // id 1..4
        toolLay->addWidget(btn);
    }
    modeGroup_->button(1)->setChecked(true);

    toolLay->addSpacing(16);
    toolLay->addWidget(new QLabel(QStringLiteral("规则")));
    ruleGroup_ = new QButtonGroup(this);
    ruleGroup_->setExclusive(true);
    auto *btnIntrusion = new QPushButton(QStringLiteral("闯入"));
    btnIntrusion->setCheckable(true);
    auto *btnCross = new QPushButton(QStringLiteral("越界"));
    btnCross->setCheckable(true);
    ruleGroup_->addButton(btnIntrusion, 1);
    ruleGroup_->addButton(btnCross, 2);
    btnIntrusion->setChecked(true);
    toolLay->addWidget(btnIntrusion);
    toolLay->addWidget(btnCross);

    toolLay->addStretch(1);
    polyCloseBtn_ = new QPushButton(QStringLiteral("闭合多边形"));
    polyUndoBtn_ = new QPushButton(QStringLiteral("撤销顶点"));
    toolLay->addWidget(polyCloseBtn_);
    toolLay->addWidget(polyUndoBtn_);
    editorLay->addLayout(toolLay);

    canvas_ = new FenceCanvas(this);
    canvas_->setMinimumSize(480, 320);
    editorLay->addWidget(canvas_, 1);
    mainLay->addWidget(editorBox, 1);

    // 右栏：版本信息（§5.2 字段）+ 区域列表
    auto *rightPanel = new QWidget;
    rightPanel->setFixedWidth(280);
    auto *rightLay = new QVBoxLayout(rightPanel);
    rightLay->setContentsMargins(0, 0, 0, 0);
    rightLay->setSpacing(8);

    auto *versionBox = new QGroupBox(QStringLiteral("版本信息"));
    auto *versionLay = new QVBoxLayout(versionBox);
    versionList_ = new QListWidget;
    versionList_->setMaximumHeight(140);
    versionLay->addWidget(versionList_);
    versionInfoLabel_ = new QLabel;
    versionInfoLabel_->setWordWrap(true);
    versionInfoLabel_->setStyleSheet(QStringLiteral("QLabel{color:#aaa;}"));
    versionLay->addWidget(versionInfoLabel_);
    framePreview_ = new QLabel;
    framePreview_->setFixedSize(240, 135);   // 16:9，与合成画面同比例
    framePreview_->setAlignment(Qt::AlignCenter);
    framePreview_->setStyleSheet(QStringLiteral(
        "QLabel{border:1px solid #444; background:#161a20; color:#888;}"));
    versionLay->addWidget(framePreview_, 0, Qt::AlignHCenter);
    rightLay->addWidget(versionBox);

    auto *regionBox = new QGroupBox(QStringLiteral("区域列表（顺序即编号）"));
    auto *regionLay = new QVBoxLayout(regionBox);
    regionList_ = new QListWidget;
    regionLay->addWidget(regionList_, 1);
    auto *moveLay = new QHBoxLayout;
    btnMoveUp_ = new QPushButton(QStringLiteral("上移"));
    btnMoveDown_ = new QPushButton(QStringLiteral("下移"));
    moveLay->addWidget(btnMoveUp_);
    moveLay->addWidget(btnMoveDown_);
    regionLay->addLayout(moveLay);
    rightLay->addWidget(regionBox, 1);
    mainLay->addWidget(rightPanel);
    root->addLayout(mainLay, 1);

    // 底部：状态提示
    statusLabel_ = new QLabel;
    statusLabel_->setStyleSheet(QStringLiteral("QLabel{color:#888;}"));
    root->addWidget(statusLabel_);

    // ---- 信号连接 ----
    connect(channelCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FenceManageWidget::onChannelChanged);
    connect(canvas_, &FenceCanvas::regionsEdited, this, &FenceManageWidget::onCanvasRegionsEdited);
    connect(canvas_, &FenceCanvas::frameRefreshRequested,
            this, &FenceManageWidget::onFrameRefreshRequested);
    connect(modeGroup_, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &FenceManageWidget::onModeButton);
    connect(ruleGroup_, QOverload<int>::of(&QButtonGroup::buttonClicked),
            this, &FenceManageWidget::onRuleButton);
    connect(regionList_, &QListWidget::currentRowChanged,
            this, [this](int) { onRegionSelected(); });
    connect(versionList_, &QListWidget::currentRowChanged,
            this, &FenceManageWidget::onVersionSelected);
    connect(btnMoveUp_, &QPushButton::clicked, this, [this] { onMoveRegion(true); });
    connect(btnMoveDown_, &QPushButton::clicked, this, [this] { onMoveRegion(false); });
    connect(polyCloseBtn_, &QPushButton::clicked, canvas_, &FenceCanvas::closePolygon);
    connect(polyUndoBtn_, &QPushButton::clicked, canvas_, &FenceCanvas::undoLastPolyPoint);
    connect(btnRefresh_, &QPushButton::clicked, this, &FenceManageWidget::onRefresh);
    connect(btnNew_, &QPushButton::clicked, this, &FenceManageWidget::onNewFence);
    connect(btnSave_, &QPushButton::clicked, this, &FenceManageWidget::onSaveDraft);
    connect(btnApply_, &QPushButton::clicked, this, &FenceManageWidget::onApply);
    connect(btnCopy_, &QPushButton::clicked, this, &FenceManageWidget::onCopyAsNew);
    connect(btnDelete_, &QPushButton::clicked, this, &FenceManageWidget::onDeleteDraft);
    connect(btnDisable_, &QPushButton::clicked, this, &FenceManageWidget::onDisable);
    connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
        dirty_ = true;
        updateStateLabel();
    });
}

// ============================================================================
// 身份换算
// ============================================================================

QString FenceManageWidget::currentCameraId() const
{
    // 协议身份恒为 "CH1".."CH4"（1-based，§7.1 口径），与下拉显示文本「通道N」解耦
    return QStringLiteral("CH%1").arg(currentChannelIndex() + 1);
}

int FenceManageWidget::currentChannelIndex() const
{
    return channelCombo_->currentIndex();  // 0-based（playerWidget 用）
}

QString FenceManageWidget::currentTaskId() const
{
    return session::ConstructionSession::instance().snapshot().taskId;
}

bool FenceManageWidget::isLoggedIn() const
{
    return session::ConstructionSession::instance().isLoggedIn();
}

// ============================================================================
// 装载与状态
// ============================================================================

void FenceManageWidget::loadFenceForChannel()
{
    currentFence_ = PadFence();
    editingVersion_ = 0;
    dirty_ = false;

    if (!isLoggedIn()) {
        clearEditor();
        applyEditorState();
        setStatus(QStringLiteral("先扫码登录，再管理围栏"));
        return;
    }

    PadFenceStore store;
    const QList<PadFence> fences = store.fencesForTask(currentTaskId());
    const QString cameraId = currentCameraId();
    for (const PadFence &fence : fences) {
        if (fence.cameraId == cameraId) {
            currentFence_ = fence;
            break;
        }
    }

    if (currentFence_.fenceId.isEmpty()) {
        clearEditor();
        applyEditorState();
        setStatus(QStringLiteral("该通道还没有围栏，点「新建围栏」开始"));
        return;
    }

    // 编辑目标：有草稿就编辑草稿；没有草稿就看生效版本（只读）；
    // 只剩停用版本时看最新那条（可「另存为新方案」再编辑），不能落在"无版本"上。
    const int draft = currentFence_.draftVersion();
    editingVersion_ = draft > 0 ? draft : currentFence_.activeVersion();
    if (editingVersion_ == 0 && !currentFence_.versions.isEmpty())
        editingVersion_ = currentFence_.versions.first().version;   // 倒序数组首条 = 最新
    refreshVersionList();

    PadFenceVersion version;
    if (currentFence_.findVersion(editingVersion_, &version)) {
        canvas_->setRegions(version.regions);
        nameEdit_->setText(version.name);
        refreshRegionList();
        refreshFramePreview();
        updateVersionInfo();
        // 登录前装载留下的"先扫码登录"提示必须在这里盖掉：
        // 本分支此前不设 status，登录后切进来旧提示一直挂着（实测踩过）。
        setStatus(versionStatusHint(version));
    } else {
        clearEditor();
        setStatus(QStringLiteral("围栏存在但没有可显示的版本"));
    }
    applyEditorState();
}

void FenceManageWidget::clearEditor()
{
    canvas_->setRegions({});
    nameEdit_->clear();
    refreshRegionList();
    refreshFramePreview();
    versionList_->clear();
    versionInfoLabel_->setText(QStringLiteral("无版本"));
}

void FenceManageWidget::applyEditorState()
{
    const bool logged = isLoggedIn();
    const bool hasFence = !currentFence_.fenceId.isEmpty();
    PadFenceVersion version;
    const bool hasVersion = hasFence && currentFence_.findVersion(editingVersion_, &version);
    const bool editable = hasVersion && version.status == PadVersionStatus::Draft;

    canvas_->setEnabled(editable);
    nameEdit_->setReadOnly(!editable);
    for (QAbstractButton *btn : modeGroup_->buttons())
        btn->setEnabled(editable);
    for (QAbstractButton *btn : ruleGroup_->buttons())
        btn->setEnabled(editable);
    polyCloseBtn_->setEnabled(editable);
    polyUndoBtn_->setEnabled(editable);

    btnNew_->setEnabled(logged && !hasFence);
    btnSave_->setEnabled(editable);
    btnApply_->setEnabled(editable);
    btnDelete_->setEnabled(editable);
    // 另存为新方案：只对非草稿版本开放（ACTIVE/DISABLED）——这正是用户定的
    // "应用后不可再编辑，只能另存为新方案"那条规则的按钮形态。
    btnCopy_->setEnabled(hasVersion && version.status != PadVersionStatus::Draft);
    btnDisable_->setEnabled(hasVersion && version.status == PadVersionStatus::Active);

    updateStateLabel();
    onRegionSelected();   // 顺带把上移/下移的可用性按当前选中行刷一遍
}

void FenceManageWidget::updateStateLabel()
{
    QString text;
    if (!isLoggedIn()) {
        text = QStringLiteral("未登录");
    } else if (currentFence_.fenceId.isEmpty()) {
        text = QStringLiteral("无围栏");
    } else {
        PadFenceVersion version;
        if (currentFence_.findVersion(editingVersion_, &version)) {
            text = QStringLiteral("%1 v%2")
                       .arg(statusDisplayName(version.status))
                       .arg(version.version);
        } else {
            text = QStringLiteral("无版本");
        }
    }
    if (dirty_)
        text += QStringLiteral("（未保存）");
    stateLabel_->setText(text);
}

void FenceManageWidget::refreshRegionList()
{
    regionList_->clear();
    const QVector<PadRegion> regions = canvas_->regions();
    for (int i = 0; i < regions.size(); ++i) {
        const PadRegion &region = regions.at(i);
        regionList_->addItem(QStringLiteral("%1 %2·%3")
                                 .arg(i + 1)
                                 .arg(ruleDisplayName(region.ruleType),
                                      shapeDisplayName(region)));
    }
}

void FenceManageWidget::refreshFramePreview()
{
    PadFenceVersion version;
    if (!currentFence_.findVersion(editingVersion_, &version)) {
        framePreview_->setText(QStringLiteral("无版本"));
        framePreview_->setPixmap(QPixmap());
        return;
    }
    // 草稿：黑底上画区域（草稿没有生效画面，§5.2 frameUrl 恒 null）。
    // 生效/停用：显示下发时刻渲染的合成画面（真实帧底图，见 onApply）。
    if (version.status == PadVersionStatus::Draft) {
        const QImage preview = geofence::renderFenceImage(version.regions);
        framePreview_->setPixmap(QPixmap::fromImage(
            preview.scaled(framePreview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        framePreview_->setToolTip(QStringLiteral("草稿预览（黑底，无生效画面）"));
        return;
    }
    if (version.framePath.isEmpty()) {
        framePreview_->setText(QStringLiteral("无生效画面"));
        framePreview_->setPixmap(QPixmap());
        return;
    }
    const QString absolute = QDir(RuntimePaths::alarmsDir()).filePath(version.framePath);
    QImage image(absolute);
    if (image.isNull()) {
        framePreview_->setText(QStringLiteral("画面文件缺失"));
        framePreview_->setPixmap(QPixmap());
        return;
    }
    framePreview_->setPixmap(QPixmap::fromImage(
        image.scaled(framePreview_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    framePreview_->setToolTip(version.framePath);
}

// ============================================================================
// 版本信息（§5.2：version/status/regions/appliedAt/createdAt/updatedAt/owner）
// ============================================================================

void FenceManageWidget::refreshVersionList()
{
    QSignalBlocker blocker(versionList_);   // 装载选中行不算"用户点选"
    versionList_->clear();
    int selectRow = -1;
    for (const PadFenceVersion &version : currentFence_.versions) {   // 已按版本号倒序
        auto *item = new QListWidgetItem(QStringLiteral("v%1 · %2")
                                             .arg(version.version)
                                             .arg(statusDisplayName(version.status)));
        item->setData(Qt::UserRole, version.version);
        versionList_->addItem(item);
        if (version.version == editingVersion_)
            selectRow = versionList_->count() - 1;
    }
    if (selectRow >= 0)
        versionList_->setCurrentRow(selectRow);
}

void FenceManageWidget::updateVersionInfo()
{
    PadFenceVersion version;
    if (!currentFence_.findVersion(editingVersion_, &version)) {
        versionInfoLabel_->setText(QStringLiteral("无版本"));
        return;
    }
    QString text = QStringLiteral("v%1 · %2\n区域 %3 个\n创建 %4\n更新 %5\n生效 %6")
                       .arg(version.version)
                       .arg(statusDisplayName(version.status))
                       .arg(version.regions.size())
                       .arg(displayTime(version.createdAt),
                            displayTime(version.updatedAt),
                            displayTime(version.appliedAt));
    const QString ownerName = version.owner.value(QStringLiteral("name")).toString();
    if (!ownerName.isEmpty())
        text += QStringLiteral("\n负责人 %1").arg(ownerName);
    versionInfoLabel_->setText(text);
}

void FenceManageWidget::onVersionSelected(int row)
{
    if (row < 0 || row >= versionList_->count())
        return;
    const int versionNumber = versionList_->item(row)->data(Qt::UserRole).toInt();
    if (versionNumber == editingVersion_)
        return;
    if (dirty_) {
        const auto answer = QtHelper::askConfirm(
            this, QStringLiteral("切换版本"),
            QStringLiteral("画布有未保存的改动，切换版本将丢弃。继续？"));
        if (answer != QMessageBox::Ok) {
            // 弹回原选中行（阻塞信号，避免再进一次本槽）
            QSignalBlocker blocker(versionList_);
            for (int i = 0; i < versionList_->count(); ++i) {
                if (versionList_->item(i)->data(Qt::UserRole).toInt() == editingVersion_) {
                    versionList_->setCurrentRow(i);
                    break;
                }
            }
            return;
        }
    }
    editingVersion_ = versionNumber;
    dirty_ = false;
    PadFenceVersion version;
    if (currentFence_.findVersion(editingVersion_, &version)) {
        canvas_->setRegions(version.regions);
        nameEdit_->setText(version.name);
        refreshRegionList();
        refreshFramePreview();
        updateVersionInfo();
        setStatus(versionStatusHint(version));
    }
    applyEditorState();
}

// ============================================================================
// 快照（§13.4.4 / §13.4.5）
// ============================================================================

QJsonObject FenceManageWidget::planSnapshot() const
{
    if (!isLoggedIn())
        return {};
    const session::ConstructionInfo info = session::ConstructionSession::instance().snapshot();
    QJsonObject plan;
    plan.insert(QStringLiteral("taskId"), info.taskId);
    plan.insert(QStringLiteral("stationName"), info.stationName);
    plan.insert(QStringLiteral("workType"), info.workType);
    plan.insert(QStringLiteral("startAt"), info.startsAt.toString(Qt::ISODate));
    plan.insert(QStringLiteral("endAt"), info.endsAt.toString(Qt::ISODate));
    return plan;
}

QJsonObject FenceManageWidget::ownerSnapshot() const
{
    if (!isLoggedIn())
        return {};
    const session::ConstructionInfo info = session::ConstructionSession::instance().snapshot();
    QJsonObject owner;
    owner.insert(QStringLiteral("id"), info.personId);
    owner.insert(QStringLiteral("name"), info.personName);
    if (!info.personRole.isEmpty())
        owner.insert(QStringLiteral("role"), info.personRole);
    return owner;
}

// ============================================================================
// 画布 / 工具栏交互
// ============================================================================

void FenceManageWidget::onChannelChanged(int)
{
    loadFenceForChannel();
}

void FenceManageWidget::onRefresh()
{
    // HTTP 下发的围栏写在同两份表里，重读库即同步。未保存改动先问一句，
    // 与切版本同口径：刷新 = 丢弃画布草稿改动，不能静默丢。
    if (dirty_) {
        const auto answer = QtHelper::askConfirm(
            this, QStringLiteral("刷新围栏"),
            QStringLiteral("画布有未保存的改动，刷新将丢弃。继续？"));
        if (answer != QMessageBox::Ok)
            return;
    }
    loadFenceForChannel();
    setStatus(QStringLiteral("已刷新，同步本机最新围栏版本"));
}

void FenceManageWidget::onCanvasRegionsEdited()
{
    dirty_ = true;
    refreshRegionList();
    updateStateLabel();
}

void FenceManageWidget::onFrameRefreshRequested()
{
    grabFrameToCanvas();
}

void FenceManageWidget::onModeButton(int id)
{
    canvas_->setMode(modeFromId(id));
}

void FenceManageWidget::onRuleButton(int id)
{
    canvas_->setCreateRule(id == 2 ? QStringLiteral("CROSS") : QStringLiteral("INTRUSION"));
}

void FenceManageWidget::onRegionSelected()
{
    const int row = regionList_->currentRow();
    const int count = regionList_->count();
    const bool editable = canvas_->isEnabled();
    btnMoveUp_->setEnabled(editable && row > 0);
    btnMoveDown_->setEnabled(editable && row >= 0 && row < count - 1);
}

void FenceManageWidget::onMoveRegion(bool up)
{
    const int row = regionList_->currentRow();
    if (row < 0)
        return;
    QVector<PadRegion> regions = canvas_->regions();
    if (regions.size() < 2)
        return;
    const int target = up ? row - 1 : row + 1;
    if (target < 0 || target >= regions.size())
        return;
    std::swap(regions[row], regions[target]);
    canvas_->setRegions(regions);
    dirty_ = true;
    refreshRegionList();
    regionList_->setCurrentRow(target);
    updateStateLabel();
}

void FenceManageWidget::grabFrameToCanvas()
{
    if (!frameGrabber_ || !canvas_->isVisible())
        return;
    canvas_->setBackground(frameGrabber_(currentChannelIndex()));
}

// ============================================================================
// 操作：新建 / 保存草稿 / 另存为新方案
// ============================================================================

void FenceManageWidget::onNewFence()
{
    if (!isLoggedIn())
        return;
    // §5.3 要求 save-draft 带 1–8 个区域，空画布会被校验拒掉、围栏永远建不出来。
    // 所以新建自带一个默认矩形（居中半幅），落库后即可拖锚点改形/删除重画。
    if (canvas_->regions().isEmpty()) {
        PadRegion region;
        region.regionId = newRegionId();
        region.shapeType = QStringLiteral("RECT");
        region.ruleType = canvas_->createRule();
        region.points = {QPointF(0.25, 0.25), QPointF(0.75, 0.25),
                         QPointF(0.75, 0.75), QPointF(0.25, 0.75)};
        canvas_->setRegions({region});
    }
    doSaveDraft(QString(), 0, false);
}

void FenceManageWidget::onSaveDraft()
{
    doSaveDraft(currentFence_.fenceId, 0, false);
}

void FenceManageWidget::onCopyAsNew()
{
    doSaveDraft(currentFence_.fenceId, editingVersion_, true);
}

void FenceManageWidget::doSaveDraft(const QString &fenceId, int sourceVersion,
                                    bool hasSourceVersion)
{
    if (!isLoggedIn())
        return;

    QVector<PadRegion> regions = canvas_->regions();
    // 新画区域的 regionId 由本机分配（Pad 生成口径：本机生成、原样保存）。
    for (PadRegion &region : regions) {
        if (region.regionId.isEmpty())
            region.regionId = newRegionId();
    }
    // 入库前跑同一份校验（HTTP 边界那层做的事，页面提前做，给中文报错）。
    QString regionError;
    if (!geofence::validateRegions(geofence::regionsJson(regions), &regions, &regionError)) {
        setStatus(QStringLiteral("区域不合法：%1").arg(regionError), true);
        return;
    }

    PadFenceStore::DraftInput input;
    input.fenceId = fenceId;
    input.taskId = currentTaskId();
    input.cameraId = currentCameraId();
    input.streamId = currentCameraId();   // §13.5：streamId 与 cameraId 同值
    input.name = nameEdit_->text().trimmed();
    if (input.name.isEmpty())
        input.name = QStringLiteral("围栏-%1").arg(channelCombo_->currentText());
    input.regions = regions;
    input.plan = planSnapshot();
    input.owner = ownerSnapshot();
    input.sourceVersion = sourceVersion;
    input.hasSourceVersion = hasSourceVersion;

    PadFenceStore store;
    QString fenceIdOut;
    int versionOut = 0;
    QString detail;
    const auto outcome = store.saveDraft(input, &fenceIdOut, &versionOut, &detail);
    if (outcome != PadFenceStore::DraftOutcome::Created
        && outcome != PadFenceStore::DraftOutcome::Edited
        && outcome != PadFenceStore::DraftOutcome::Copied) {
        setStatus(QStringLiteral("保存失败：%1").arg(draftOutcomeText(outcome, detail)), true);
        return;
    }

    loadFenceForChannel();
    QString action;
    switch (outcome) {
    case PadFenceStore::DraftOutcome::Created:
        action = QStringLiteral("已创建草稿围栏 v%1，在画布上绘制区域").arg(versionOut);
        break;
    case PadFenceStore::DraftOutcome::Edited:
        action = QStringLiteral("草稿已保存（版本号 v%1 不变）").arg(versionOut);
        break;
    case PadFenceStore::DraftOutcome::Copied:
        action = QStringLiteral("已另存为新草稿 v%1，可继续编辑").arg(versionOut);
        break;
    default:
        break;
    }
    setStatus(action);
}

// ============================================================================
// 操作：应用（与 pad_http_fences.cpp 的 apply 路由同一套顺序）
// ============================================================================

void FenceManageWidget::onApply()
{
    PadFenceVersion version;
    if (!currentFence_.findVersion(editingVersion_, &version)
        || version.status != PadVersionStatus::Draft) {
        return;
    }
    if (dirty_) {
        setStatus(QStringLiteral("画布有未保存的改动，先保存草稿再应用"), true);
        return;
    }

    // 先渲染合成画面：画不出 PNG 就不下发（对应 HTTP 的 503/3003）。
    // 底图取本通道当前帧——生效画面要带下发时刻的真实场景（2026-11-17 用户裁定）；
    // 抓不到帧时渲染器退回网格底，不伪造画面。
    QImage background;
    if (frameGrabber_)
        background = frameGrabber_(currentChannelIndex());
    QString absolutePath, relativePath, renderError;
    if (!geofence::renderFenceFrame(currentFence_.fenceId, editingVersion_, version.regions,
                                    RuntimePaths::alarmsDir(), QStringLiteral("fence-frames"),
                                    &absolutePath, &relativePath, &renderError, background)) {
        setStatus(QStringLiteral("生效画面渲染失败：%1").arg(renderError), true);
        return;
    }

    PadFenceStore store;
    QString appliedAt, detail;
    const auto outcome = store.applyDraft(currentFence_.fenceId, editingVersion_,
                                          &appliedAt, &detail);
    if (outcome != PadFenceStore::ApplyOutcome::Applied) {
        QString reason;
        switch (outcome) {
        case PadFenceStore::ApplyOutcome::FenceNotFound:
            reason = QStringLiteral("本机没有这条围栏");
            break;
        case PadFenceStore::ApplyOutcome::WrongVersion:
            reason = QStringLiteral("这条围栏里没有那个版本");
            break;
        case PadFenceStore::ApplyOutcome::NotADraft:
            reason = QStringLiteral("只有草稿能下发");
            break;
        default:
            reason = QStringLiteral("下发没写进库：%1").arg(detail);
            break;
        }
        setStatus(QStringLiteral("应用失败：%1").arg(reason), true);
        return;
    }
    if (!store.setFramePath(currentFence_.fenceId, editingVersion_, relativePath)) {
        setStatus(QStringLiteral("已下发，但生效画面没记进库"), true);
        return;
    }
    // 让这一版立刻参与检测（检测读的是 FenceManager 的内存快照）。
    geofence::FenceManager::instance().refreshFromStores();
    loadFenceForChannel();
    setStatus(QStringLiteral("已应用 v%1，原生效版本已停用").arg(editingVersion_));
}

// ============================================================================
// 操作：删除草稿 / 停用
// ============================================================================

void FenceManageWidget::onDeleteDraft()
{
    PadFenceVersion version;
    if (!currentFence_.findVersion(editingVersion_, &version)
        || version.status != PadVersionStatus::Draft) {
        return;
    }
    const auto answer = QtHelper::askConfirm(
        this, QStringLiteral("删除草稿"),
        QStringLiteral("删除草稿 v%1？删除后不可恢复。").arg(editingVersion_));
    if (answer != QMessageBox::Ok)
        return;

    PadFenceStore store;
    QString detail;
    const auto outcome = store.deleteDraft(currentFence_.fenceId, editingVersion_, &detail);
    if (outcome != PadFenceStore::DeleteOutcome::Deleted) {
        setStatus(QStringLiteral("删除失败：%1").arg(detail), true);
        return;
    }
    loadFenceForChannel();
    setStatus(QStringLiteral("草稿已删除"));
}

void FenceManageWidget::onDisable()
{
    PadFenceVersion version;
    if (!currentFence_.findVersion(editingVersion_, &version)
        || version.status != PadVersionStatus::Active) {
        return;
    }
    const auto answer = QtHelper::askConfirm(
        this, QStringLiteral("停用围栏"),
        QStringLiteral("停用生效版本 v%1？停用是终态，之后只能另存为新方案再改。")
            .arg(editingVersion_));
    if (answer != QMessageBox::Ok)
        return;

    PadFenceStore store;
    QString disabledAt, detail;
    const auto outcome = store.disableVersion(currentFence_.fenceId, editingVersion_,
                                              &disabledAt, &detail);
    if (outcome != PadFenceStore::DisableOutcome::Disabled) {
        setStatus(QStringLiteral("停用失败：%1").arg(detail), true);
        return;
    }
    geofence::FenceManager::instance().refreshFromStores();
    loadFenceForChannel();
    setStatus(QStringLiteral("已停用 v%1（终态，只能另存为新方案再改）").arg(editingVersion_));
}

// ============================================================================
// 状态提示
// ============================================================================

void FenceManageWidget::setStatus(const QString &text, bool isError)
{
    statusLabel_->setText(text);
    statusLabel_->setStyleSheet(isError ? QStringLiteral("QLabel{color:#e53935;}")
                                        : QStringLiteral("QLabel{color:#888;}"));
}
