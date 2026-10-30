#pragma once

// ============================================================================
// construction_session.h - 施工信息与登录会话
// ============================================================================
//
// 职责：
//   1. 定义"施工信息"的字段契约（ConstructionInfo）
//   2. 解析并校验二维码文本（fromJson / fromQrText，校验只有这一处）
//   3. 保存当前登录会话的施工信息（单例 + QReadWriteLock）
//
// **改字段只动这个文件**：加/减/改键名，只需改 ConstructionInfo 的成员、
// toJson()、fromJson() 三处。落库（construction_info 那一列存的是整份 JSON）、
// 报警详情框（按 JSON 逐键渲染）、WebSocket 上行（data["construction"] 子对象）
// 都不需要跟着改。
//
// 使用方式：
//   ConstructionSession::instance().startLogin(qrText, &err);   // 登录页
//   ConstructionSession::instance().snapshot();                 // 打戳/显示方
//
// 线程安全：
//   写：UI 线程（startLogin/logout）
//   读：解码线程也会调 snapshot()（makeAlarmRecord 里打戳），按值返回，
//       调用方不得持锁跨数据库写。
//
// ============================================================================

#include <QJsonObject>
#include <QReadWriteLock>
#include <QString>

namespace session {

// 一条施工信息。空对象表示"未登录"。
struct ConstructionInfo {
    // 键名（二维码里的 JSON 键）——与字段一一对应，集中列在这里便于对照协议文档
    static const char kKeyConstructionId[];  // "construction_id"
    static const char kKeyWorkerName[];      // "worker_name"
    static const char kKeyWorkerPhone[];     // "worker_phone"
    static const char kKeyWorkerId[];        // "worker_id"
    static const char kKeyWorkContent[];     // "work_content"

    QString constructionId;  // 施工id，必填
    QString workerName;      // 负责人姓名，必填
    QString workerPhone;     // 负责人电话，可空
    QString workerId;        // 负责人工号，可空
    QString workContent;     // 施工工作内容，可空

    QJsonObject raw;         // 码里的完整载荷，原样留着（详情框与上行都从它渲染）

    bool isEmpty() const;

    // 落库与上行用的规范化 JSON：以 raw 为准，再覆盖上五个具名字段，
    // 保证"具名字段改了名也能原样透传"且不会丢未知键。
    QJsonObject toJson() const;

    // 解析 + 校验。失败返回 false，err 给出中文原因（缺哪个字段点出哪个）。
    // 校验规则只有这一处：construction_id 与 worker_name 缺一即拒绝登录，
    // 其余字段可空，界面显示 "—"。
    static bool fromJson(const QJsonObject &obj, ConstructionInfo *out, QString *err);

    // 二维码原文（期望是 JSON）→ fromJson。非 JSON / 空 / 缺字段都返回 false。
    static bool fromQrText(const QString &qrText, ConstructionInfo *out, QString *err);

    // 界面用的显示文本：空串统一成 "—"，避免出现空白行。
    static QString display(const QString &value);
};

class ConstructionSession {
public:
    static ConstructionSession &instance();

    // 解析 qrText 并校验；成功则把这份信息**定死**为当前会话内容，返回 true。
    // 失败返回 false 并填 err，会话保持原状（不会半成品式地半登录）。
    bool startLogin(const QString &qrText, QString *err);

    // 直接以已解析的对象登录（单测与"测试登录"按钮走 fromQrText 同一条路径用）
    bool startLogin(const ConstructionInfo &info);

    void logout();

    bool isLoggedIn() const;

    // 按值返回当前会话快照；未登录返回空对象。
    ConstructionInfo snapshot() const;

    // 便于日志与详情显示的简短一行摘要，不含任何二进制内容。
    QString summary() const;

private:
    ConstructionSession() = default;
    Q_DISABLE_COPY(ConstructionSession)

    mutable QReadWriteLock lock_;
    ConstructionInfo info_;
    bool loggedIn_ = false;
};

} // namespace session
