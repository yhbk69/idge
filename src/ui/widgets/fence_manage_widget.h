#pragma once

// ============================================================================
// fence_manage_widget.h - 电子围栏管理页（新 Tab，§5 围栏的本地管理界面）
// ============================================================================
//
// 管理的是 Pad 围栏（PadFenceStore 那两张表）：按 施工任务(taskId) + 摄像头
// (cameraId) 一条围栏、多版本（ACTIVE/DRAFT/DISABLED）。本机手绘围栏
// （native_fences）不在这里管；视频页只显示围栏，不再提供绘制入口。
//
// 布局：顶栏 = 通道/名称/状态 + 全部生命周期按钮；主区左 = 编辑画布，
// 右 = 版本信息（版本列表 + §5.2 字段明细 + 生效画面预览）+ 区域列表。
// 版本列表点选可回看任意历史版本（只读），草稿仍是默认编辑目标。
//
// 生命周期（用户定的两条，与 PadFenceStore 的状态机一一对应，页面只负责
// 把状态翻译成可用的按钮，不自己造状态）：
//   1) 围栏创建之后：保存草稿 / 编辑 / 应用
//   2) 围栏应用之后：不可再编辑，只能另存为新方案后编辑
//   于是：
//   - DRAFT  → 画布可编辑，按钮 = 保存草稿 / 应用 / 删除草稿
//   - ACTIVE → 画布只读，按钮 = 另存为新方案 / 停用
//   - DISABLED → 画布只读，按钮 = 另存为新方案（停用是终态，只能复制出新草稿再改）
//
// 编辑画布（FenceCanvas）的改动只在点「保存草稿」时落库——草稿语义：
// 画布上改多少遍都不产生版本号变化，save-draft 是就地覆盖（Edited）。
//
// 摄像头口径：下拉显示「通道1..通道4」，协议身份仍是 CH1..CH4（§7.1
// channelIdentity() 的 1-based，currentCameraId 按下标换算），同时充当
// §13.5 的 streamId（pad_http_api.cpp 里两者同值）；画布背景帧从视频页
// 对应通道的解码器抓（复用，不另开一路流）。
//
// ============================================================================

#include <QImage>
#include <QJsonObject>
#include <QWidget>

#include <functional>

#include "pad_fence_store.h"   // PadFence：本页管理的数据

class QButtonGroup;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

namespace geofence {
class FenceCanvas;
}

class FenceManageWidget : public QWidget {
    Q_OBJECT
public:
    explicit FenceManageWidget(QWidget *parent = nullptr);

    // 抓帧回调：入参是 0-based 通道号，返回该通道当前帧（空图 = 没帧）。
    // 由主窗口注入（内部走视频页对应通道的解码器，复用不另开流）。做成回调
    // 而不是直接握 frmVideoWindow 指针：页面因此不依赖视频/FFmpeg 那条链，
    // 离屏自检能单独编它。
    void setFrameGrabber(std::function<QImage(int channel)> grabber);

    // 重新从库里装载当前通道的围栏（切到本页 / 登录成功后调）。
    void refresh();

private slots:
    void onChannelChanged(int index);
    void onRefresh();
    void onCanvasRegionsEdited();
    void onFrameRefreshRequested();
    void onModeButton(int id);
    void onRuleButton(int id);
    void onRegionSelected();
    void onMoveRegion(bool up);
    void onVersionSelected(int row);
    void onNewFence();
    void onSaveDraft();
    void onApply();
    void onCopyAsNew();
    void onDeleteDraft();
    void onDisable();

private:
    void buildUi();

    // 身份换算
    QString currentCameraId() const;    // 协议身份："CH1".."CH4"（1-based，按下标换算）
    int currentChannelIndex() const;    // 0-based（playerWidget 用）
    QString currentTaskId() const;      // 当前施工会话的任务 ID
    bool isLoggedIn() const;

    // 装载与状态
    void loadFenceForChannel();         // 按当前通道读库 → 画布/名称/预览/按钮
    void clearEditor();
    void applyEditorState();            // 按当前版本状态设置只读与按钮可用性
    void updateStateLabel();            // 状态标签（草稿/生效/已停用 + 未保存标记）
    void refreshRegionList();
    void refreshFramePreview();
    void refreshVersionList();          // 版本列表（倒序），选中行 = editingVersion_
    void updateVersionInfo();           // 当前版本的 §5.2 字段明细（时间/负责人/区域数）

    // §13.4.4 / §13.4.5 的快照（存进去什么形状就出什么形状，本机不补不重排）
    QJsonObject planSnapshot() const;
    QJsonObject ownerSnapshot() const;

    // 操作（与 pad_http_fences.cpp 各路由同一套顺序与判据）
    void doSaveDraft(const QString &fenceId, int sourceVersion, bool hasSourceVersion);

    void setStatus(const QString &text, bool isError = false);
    void grabFrameToCanvas();

    // ---- 控件 ----
    QComboBox *channelCombo_ = nullptr;
    QLabel *stateLabel_ = nullptr;      // 当前版本状态（草稿 v2 / 生效 v1 / …）
    QLineEdit *nameEdit_ = nullptr;
    QLabel *framePreview_ = nullptr;    // 画面预览：草稿=黑底区域图，生效/停用=下发时刻合成画面

    QButtonGroup *modeGroup_ = nullptr;
    QButtonGroup *ruleGroup_ = nullptr;
    QPushButton *polyUndoBtn_ = nullptr;
    QPushButton *polyCancelBtn_ = nullptr;

    geofence::FenceCanvas *canvas_ = nullptr;
    QListWidget *regionList_ = nullptr;
    QPushButton *btnMoveUp_ = nullptr;
    QPushButton *btnMoveDown_ = nullptr;

    QListWidget *versionList_ = nullptr;    // 全部版本（倒序），点选切换查看/编辑目标
    QLabel *versionInfoLabel_ = nullptr;    // 当前版本的 §5.2 字段明细

    QPushButton *btnRefresh_ = nullptr;   // 重读库，同步 HTTP 下发的围栏版本
    QPushButton *btnNew_ = nullptr;
    QPushButton *btnSave_ = nullptr;
    QPushButton *btnApply_ = nullptr;
    QPushButton *btnCopy_ = nullptr;
    QPushButton *btnDelete_ = nullptr;
    QPushButton *btnDisable_ = nullptr;
    QLabel *statusLabel_ = nullptr;

    // ---- 状态 ----
    geofence::PadFence currentFence_;  // 当前通道的围栏（含全部版本，倒序）
    int editingVersion_ = 0;           // 画布上显示/编辑的版本号；0 = 无
    bool dirty_ = false;               // 画布相对已保存草稿有改动
    std::function<QImage(int)> frameGrabber_;   // 背景帧来源（主窗口注入）
};
