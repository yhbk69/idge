#ifndef PAD_FENCE_STORE_H
#define PAD_FENCE_STORE_H

// ============================================================================
// pad_fence_store.h — v1 §5 围栏：版本行存储、状态机与围栏对象 JSON
// ============================================================================
//
// 与同目录那三个文件的关系先说清，因为它们**不是同一套围栏**：
//   fence_shape.h / fence_manager.h = 本机自己的围栏：按**通道号**组织、存在 config.json 的
//       geofence 段、画在四宫格上、判定由 FenceChecker 用。
//   本文件 = Pad 接口 v1 的围栏：按 **taskId + cameraId** 组织、存在 idge.db 的两张表里、
//       有多版本（ACTIVE/DRAFT/DISABLED）、画面由 Pad 自己显示。
// ⚠ 两套**没有接起来**：Pad 下发一条围栏不会让本机开始按它判定，本机界面画的围栏也不会出现在
//   GET /fences 里。这不是遗漏而是本轮的范围决定（§5 通篇讲的是"围栏的增删改查契约"，
//   没有任何一条要求它驱动检测），但**它是个真实的功能缺口**，
//   已在 src/biz/pad/README.md 的"待对方确认"里挂着，别读到一半以为已经打通了。
//
// 为什么存库不存 config.json：§5.2 的模型是"一个围栏多条版本行 + 每行 1~8 个区域 +
// 每行一份 plan/owner 快照"，那是关系形状；塞进 config.json 会变成一个手改极易写坏的深嵌套，
// 而 config.json 同时还承担"启动就能读"的职责。
//
// 为什么 regionId 用**原样 JSON 文本**存整片 regions：规约明写"regionId 由 Pad 生成、
// 边缘设备必须原样保存、不生成不改写"。拆开一行一区域就得重建 JSON，
// 重建时键序/数字写法任何一处漂了，Pad 那边"同一个区域"就不再是同一个区域。
//
// 线程：与 AlarmDAO 同一口径——DatabaseManager 按线程分配连接，
// 每个方法自己取当前线程的连接，所以**只在服务端所在线程（GUI 线程）用**。
// 状态机（§5.3 那五种状态组合）整体是"读几条 → 判 → 写一条"，
// 没有事务包裹：单写线程下成立；将来要支持云端并发写再补 BEGIN IMMEDIATE。
// ============================================================================

#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QPointF>
#include <QString>
#include <QVector>

namespace geofence {

// ============================================================================
// 版本状态（§13.4.2 的三值）
// ============================================================================
enum class PadVersionStatus { Active, Draft, Disabled };

/// ACTIVE / DRAFT / DISABLED
QString padStatusName(PadVersionStatus status);
/// 认不了返回 false（库里读到脏值时用，绝不"就当草稿"猜）
bool padStatusFromName(const QString &text, PadVersionStatus *out);

// ============================================================================
// Region — §13.4.3 的一个围栏区域
// ============================================================================
struct PadRegion {
    QString regionId;             ///< Pad 生成、本机原样保存
    QString shapeType;            ///< RECT / POLYGON（原文存着，出 JSON 时不改写）
    QVector<QPointF> points;      ///< 归一化 0~1 顶点
    QString ruleType;             ///< INTRUSION / CROSS

    QJsonObject toJson() const;
};

// ============================================================================
// PadFenceVersion / PadFence — §13.4.1、§13.4.2
// ============================================================================
struct PadFenceVersion {
    int version = 0;
    PadVersionStatus status = PadVersionStatus::Draft;
    QString name;
    QVector<PadRegion> regions;
    QString framePath;       ///< 相对媒体根的 PNG 路径；**只有 ACTIVE/DISABLED 可能有值**
    QString appliedAt;       ///< 内部定宽文本；草稿为空
    QString createdAt;
    QString updatedAt;
    QJsonObject plan;        ///< §13.4.4 快照，原样存原样出
    QJsonObject owner;       ///< §13.4.5 快照
};

struct PadFence {
    QString fenceId;
    QString taskId;
    QString cameraId;
    QString streamId;
    QVector<PadFenceVersion> versions;   ///< 按版本号**倒序**（§5.2）

    /// 当前 ACTIVE 的版本号；没有返回 0（§13.4.1 就是这个语义，不是"未知"）
    int activeVersion() const;
    int draftVersion() const;
    /// 某个版本号的行；找不到返回 false。倒序数组里线性找：一个围栏的版本数是个位数。
    bool findVersion(int version, PadFenceVersion *out) const;
};

// ============================================================================
// §5.2 的 regions 校验（唯一一份，HTTP 与自测都走它）
// ============================================================================
/// §5.2 的 regions 校验（唯一一份，HTTP 与自测都走它）
/// —— 声明见下面 validateRegions 的注释块。

/**
 * @param raw      对端给的 regions 数组
 * @param out      [out] 解出来的区域（regionId 原样保留）
 * @param err      [out] 中文原因，直接进 §2.4 信封的 detail
 * @return false = 非法。调用方一律回 **422 / 4001**（§5.3 与 §2.4 在这一档是一致的）
 *
 * 逐条判据都写在实现里的注释上，共八条：数量 1~8、regionId 非空且不重复、
 * shapeType 只有 RECT/POLYGON、RECT 恰好 4 点、POLYGON 3~20 点、
 * 每点两个数、坐标 0~1、ruleType 只有 INTRUSION/CROSS。
 * ⚠ 闭合/自交**不校验**：§5.2 明写"Pad 负责闭合/自交校验"，本机重复实现一遍
 *   只会得到两套判据不一致时谁对的问题。
 */
bool validateRegions(const QJsonArray &raw, QVector<PadRegion> *out, QString *err);

/// regions → §5.2 那个数组的 JSON 形状（内存对象转出去，**时间字段不在这里**，
/// 所以本函数与下面的 toJsonText 都不碰 wireTime；协议出口的时间换算在 pad_http_fences.cpp）
QJsonArray regionsJson(const QVector<PadRegion> &regions);
/// 存进 regions_json 列的紧凑文本（读回来还是同一份，见文件头"为什么原样存"那段）
QString regionsToJsonText(const QVector<PadRegion> &regions);
/// 从 regions_json 列读回内存。库里那串坏了就返回空表并给原因——**不猜**
bool regionsFromJsonText(const QString &text, QVector<PadRegion> *out, QString *err);

// ============================================================================
// PadFenceStore — 两张表的读写 + §5.3 的状态机
// ============================================================================
class PadFenceStore
{
public:
    // 本类没有任何成员变量（每个方法自己取当前线程的库连接，理由见文件头的线程那段），
    // 所以连写操作也一律 const：路由把它们装进按值捕获的 lambda 时，非 const 版本会因为
    // lambda 默认不 mutable 而编译不过（gcc13 报 discards qualifiers，实测踩过）。
    /// §5.3 GET /fences?taskId= 的返回（没有围栏时是空表，不是"未打开库"）
    QList<PadFence> fencesForTask(const QString &taskId) const;
    /// §5.3 GET /fences/{fenceId}；false = 没有这条围栏（调用方回 404/2002）
    bool loadFence(const QString &fenceId, PadFence *out) const;

    /// 一次 save-draft 的入参（已经从 JSON 里解好并验过形状）
    struct DraftInput {
        QString fenceId;              ///< 空 = 新建；非空 = 编辑草稿或复制版本
        QString taskId;
        QString cameraId;
        QString streamId;
        QString name;
        QVector<PadRegion> regions;
        QJsonObject plan;
        QJsonObject owner;
        int sourceVersion = 0;        ///< 复制来源；配合 hasSourceVersion 用
        bool hasSourceVersion = false;
    };

    /// save-draft 的九种结果。前三个是成功，后六个各自对应 §5.3/§2.4 的一个码，
    /// 由 HTTP 层翻译成信封——**这里不出现 HTTP 概念**，因为自测要能不看协议就跑状态机。
    enum class DraftOutcome {
        Created,               ///< 新围栏 + version=1 草稿
        Edited,                ///< 原草稿就地覆盖，版本号不变
        Copied,                ///< 从 sourceVersion 复制出 max+1 的新草稿
        FenceNotFound,         ///< → 404 / 2002
        DraftAlreadyExists,    ///< → 409 / 2004（该摄像头已存在草稿）
        NotDraftToEdit,        ///< → 409 / 2005（没有可编辑的草稿；带 fenceId 却没带
                               ///  sourceVersion 且无草稿就是这一档，§5.3 表里那行也是 2005）
        SourceVersionNotFound, ///< → 404 / 2002（带的那个版本号在这条围栏里没有）
        CameraMismatch,        ///< → 409 / 2006（cameraId/taskId 与围栏不一致，
                               ///  或新建时该摄像头已有围栏不能重复建）
        StoreError,            ///< → 500 / 3001
    };

    /**
     * @brief §5.3 的 save-draft：新建 / 编辑草稿 / 复制版本，三合一
     * @param fenceIdOut/versionOut [out] 成功时写；失败时不保证被写
     * @param detail [out] 失败时的中文补充（进信封 detail）
     */
    DraftOutcome saveDraft(const DraftInput &input, QString *fenceIdOut, int *versionOut,
                           QString *detail) const;

    /// §5.3 apply 的前置判定与落库（**不含渲染画面**：渲染由调用方做，
    /// 因为"画不出 PNG"是存储/证据不可用（503/3003），不该由围栏库来判）。
    /// 成功后：目标版本 → ACTIVE + applied_at，原 ACTIVE → DISABLED，framePath 由调用方
    /// 之后再调 setFramePath() 写进去。
    enum class ApplyOutcome { Applied, FenceNotFound, NotADraft, WrongVersion, StoreError };
    ApplyOutcome applyDraft(const QString &fenceId, int version, QString *appliedAtOut,
                            QString *detail) const;
    /// 把渲染好的 PNG 相对路径写进那一版（apply 成功后紧接着调）
    bool setFramePath(const QString &fenceId, int version, const QString &framePath) const;

    /// §5.3 disable：只有 ACTIVE 可停用，停用是终态
    enum class DisableOutcome { Disabled, FenceNotFound, NotActive, WrongVersion, StoreError };
    DisableOutcome disableVersion(const QString &fenceId, int version, QString *disabledAtOut,
                                  QString *detail) const;

    /// §5.3 delete-draft：只删草稿行；同 (fenceId,version) 之外一条都不动
    enum class DeleteOutcome { Deleted, FenceNotFound, NotADraft, WrongVersion, StoreError };
    DeleteOutcome deleteDraft(const QString &fenceId, int version, QString *detail) const;

    /// 库里围栏总数（诊断与自测用）
    int fenceCount() const;

private:
    /// 读一条围栏的版本行（不含主表字段）
    QVector<PadFenceVersion> loadVersions(const QString &fenceId) const;
    /// 按 §5.1 那个唯一键查围栏（新建前的"已存在吗"判定用它；fenceId 不存在返回 false）
    bool loadFenceByTaskCamera(const QString &taskId, const QString &cameraId,
                               PadFence *out) const;
    /// 分配下一个 fenceId：FENCE-<自增号>，取现有可能号的最大值 +1。
    /// **永不复用**：删光也不会退回 001（§13.4.1 要求 fenceId 稳定）。
    QString nextFenceId() const;
};

}  // namespace geofence

#endif  // PAD_FENCE_STORE_H
