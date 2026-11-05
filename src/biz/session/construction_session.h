#pragma once

// ============================================================================
// construction_session.h - 施工信息与登录会话（二维码契约 qr-login-edge-v1）
// ============================================================================
//
// 职责：
//   1. 定义"施工信息"的字段契约（ConstructionInfo）
//   2. 解析并校验二维码文本（fromJson / fromQrText，校验只有这一处）
//   3. 保存当前登录会话的施工信息（单例 + QReadWriteLock）
//
// **契约出处**：plan/qr-login-edge-v1.md（2026-11-04），示例载荷 plan/qr-login.json。
// 形状是嵌套的：`person{}` + `plan{}`，顶层另有 issuedAt/expiresAt/signatureAlg/signature。
// 旧扁平形状（construction_id/worker_name/worker_phone/worker_id/work_content）只作为
// **历史库的读取兼容**保留：alarms.construction_info 里已经躺着这种 JSON 的行不再改写，
// 所以 taskIdFromJsonText 与 fields() 都必须能读两代形状。
//
// **改字段只动这个文件**：加/减/改键名，只需改 ConstructionInfo 的成员、readObject()、
// toJson()、fields() 三处（fields() 是那个键的界面中文名与显示顺序）。
// 落库（construction_info 那一列存的是整份 JSON）、报警详情框、施工详情框、
// WebSocket 上行（data["construction"] 子对象）都不需要跟着改。
//
// 使用方式：
//   ConstructionSession::instance().setLoginPolicy(policy);      // 启动时从配置装入
//   ConstructionSession::instance().startLogin(qrText, &err);   // 登录页
//   ConstructionSession::instance().snapshot();                 // 打戳/显示方
//
// 线程安全：
//   写：UI 线程（startLogin/logout）
//   读：解码线程也会调 snapshot()（makeAlarmRecord 里打戳），按值返回，
//       调用方不得持锁跨数据库写。
//
// ============================================================================

#include <QDateTime>
#include <QJsonObject>
#include <QReadWriteLock>
#include <QString>
#include <QStringList>
#include <QVector>

namespace session {

// 一行显示用的字段：label 是界面用中文名（未知键就是键名本身），value 已过 display()。
// 之所以放在这层：详情框按 JSON 逐键渲染的"键→中文名 + 顺序"也是字段契约的一部分，
// 放在某个对话框的 .cpp 里就等于契约有两处。
struct ConstructionField {
    QString label;
    QString value;
};

// 校验档（qr-login-edge-v1.md §6）。**§6.4 有效期与 §6.5 验签暂不实现**
// （2026-11-04 裁定：先不做过期校验和签名校验）——issuedAt/expiresAt/signatureAlg/
// signature 仍照原样解析保存（§8 要求会话保留签发/过期时间），只是不据此拒绝登录。
// 现在只剩 §6.6 这一条：plan.taskId 与本机当前计划比对。
struct LoginPolicy {
    QString expectedTaskId;  // 非空 = plan.taskId 必须等于它（§6.6），空 = 不比对
};

// 一条施工信息。空对象表示"未登录"。
struct ConstructionInfo {
    // ---- 顶层信封 ----
    QString issuedAt;      // 签发时间（带时区 ISO-8601，原样保存）
    QString expiresAt;     // 过期时间
    QString signatureAlg;  // 生产固定 "ECDSA_P256_SHA256"
    QString signature;     // Base64(DER)

    // ---- person（人员，只保留协议允许的三项）----
    QString personId;    // 工号/人员 ID，必填
    QString personName;  // 姓名，必填
    QString personRole;  // 角色，可空（当前用 SITE_LEAD）

    // ---- plan（施工计划）----
    QString taskId;       // 计划 ID，必填；就是 v1 里那个 taskId（B2 定案）
    QString lineName;     // 线路
    QString stationName;  // 车站
    QString workType;     // 作业内容
    int year = 0;         // 计划年份
    int month = 0;        // 计划月份
    QStringList days;     // 计划日期；只取第一个非空值（§5.3）
    int tendays = 0;      // 旬：1/2/3，仅业务元数据
    QString starttime;    // HH:mm；空则按 constrainttime 推导
    bool startmorrow = false;
    QString constrainttime;  // "4" = 运营结束后 → 23:00
    QString endtime;         // HH:mm，必填
    bool endmorrow = false;
    int totalPersonCount = 0;  // 计划总人数，必须 > 0

    // ---- §5 推导结果（+08:00 固定时区，与 Pad 必须一致）----
    QDateTime startsAt;
    QDateTime endsAt;

    QJsonObject raw;  // 码里的完整载荷，原样留着（未知键透传用）

    bool isEmpty() const;

    // 落库与上行用的规范化 JSON：写成**新契约的嵌套形状**，未知键原样透传。
    // 旧扁平键不会再写出去（读得进、写不出），这是刻意的：新行只应有一种形状。
    QJsonObject toJson() const;

    // 解析 + 校验（qr-login-edge-v1.md §6 的 1–6 步，失败只报第一条）。
    // err 是 §9 表里的中文提示，调用方直接展示。
    static bool fromJson(const QJsonObject &obj, ConstructionInfo *out, QString *err,
                         const LoginPolicy &policy = LoginPolicy());

    // 二维码原文（期望是 JSON）→ fromJson。非 JSON / 空 / 缺字段都返回 false。
    static bool fromQrText(const QString &qrText, ConstructionInfo *out, QString *err,
                           const LoginPolicy &policy = LoginPolicy());

    // 从**已落库那段 JSON 文本**里取施工计划 ID（＝ v1 的 taskId，B2 定案同物）。
    // 新形状读 plan.taskId，旧扁平形状回落 construction_id——历史行不会因为换契约失联。
    // DAO 写入兜底、启动回填老行、Pad 出数三边都调它。
    // 空文本 / 解不出 / 不是对象 → 返回空串，含义是"这一行没有施工计划身份"。
    static QString taskIdFromJsonText(const QString &jsonText);

    // 界面用的显示文本：空串统一成 "—"，避免出现空白行。
    static QString display(const QString &value);

    // 计划窗口的一行文本："2026-11-04 23:00 → 2026-11-05 03:30"；推导失败返回空串。
    QString planWindowText() const;

    // 逐键渲染成显示行（两个详情框共用）。已知键按固定顺序给中文名，
    // 未知键原样用键名排后面（码里多带什么都不碍显示）。空对象返回空表。
    static QVector<ConstructionField> fields(const QJsonObject &obj);

    // 落库/上行里存的那段 JSON 文本 → fields()。解析失败或空串返回空表，不报错。
    static QVector<ConstructionField> fieldsFromJson(const QString &json);
};

class ConstructionSession {
public:
    static ConstructionSession &instance();

    // 启动装配时装入生产/联调档；不装 = 联调档（LoginPolicy 的默认值）。
    void setLoginPolicy(const LoginPolicy &policy);
    LoginPolicy loginPolicy() const;

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
    LoginPolicy policy_;
    bool loggedIn_ = false;
};

} // namespace session
