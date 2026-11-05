#ifndef PAD_WIRE_H
#define PAD_WIRE_H

// ============================================================================
// pad_wire.h — AlarmRecord ↔ Pad 接口 v1 的 `alarm` / `alarm_update` JSON
// ============================================================================
//
// 为什么单独一个文件：这份映射是**协议边界**，散到 WS 服务端、HTTP 层、UI 里就会变成三处，
// 而 v1 要求同一 alarmId 的合并只看 updatedAt（§3.5 末条），三处口径不一致对端就会时而看到
// PENDING 时而看到 DISPOSED。所有出站报警 JSON 只能从这里出。
//
// 四条口径要先看清（都是"规约没写死、这里必须选一个"的地方，理由写在 .cpp 对应函数上）：
//   汇总口径与"不同意时改哪一处"见 plan/口径确认单_2026-11-03.md；这里只列影响这份映射的。
//   1) 内部四态 → v1 三值（§3.9）：`pending`/`disposed`/`false_alarm` 分别出
//      `PENDING`/`DISPOSED`/`FALSE_ALARM`，其余（含本机特有的 `rectified`）一律 `PENDING`。
//      `rectified` 是本机"确认"按钮写的"我看过了"，**不等于** `DISPOSED`；已处置只能由
//      `AlarmDAO::dispose()` 写 `status='disposed'` 产生（2026-11-04 定，报告 N2）。
//      已读侧照 §3.6 的读法走：读一条报警不改状态，所以 `rectified` 出站是
//      **PENDING + readAt**，处置信息一概不臆造。
//   2) `taskId` ≡ 本机施工计划 ID（B2 定案：对外只叫 taskId）。扫码载荷自 2026-11-04 起
//      是 qr-login-edge-v1 的嵌套形状，计划 ID 在 `plan.taskId`；库里它是 `alarms.task_id`
//      一等列，所以 `taskIdOf()` 优先读列、列空才回落解 JSON（历史老行 / 还没落库的行）。
//      未登录期间产生的报警 `taskId` 是空串（回填收尾把它写成 `''`，不是 NULL）：
//      它不属于任何施工计划，对端按 taskId 过滤时看不到它——这是事实，不是缺陷。
//   3) v1 的 `alarm_update` 原本不带证据字段（§13.2.2 只有 status/readAt/dispose/taskId/
//      updatedAt/syncState），而本机报警那一刻视频片段还没落盘（ClipWriter 是"报警后 2 秒"才写）。
//      → 现场 2026-11-05 指定的流程：报警当刻先发 `alarm`（信息 + 抓拍图，clipUrl=null），
//        片段写完后发一条 `alarm_update`，里面带 `clipUrl`（本机对 §13.2.2 的一处补充，
//        规约表格待对端补上）。**不发 null**——§13.2.2"未出现的字段表示未提供"，
//        发 null 会把对端已拿到的视频清掉。
//   4) `hello_ack.deviceId` 必填而配置出厂为空 → `defaultDeviceId()` 按 /etc/machine-id 派生，
//      配置填了才用配置的（口径 A17，见下方函数注释）。
//
// 分层：biz/pad → biz/alarm 是同层合法边；URL 拼装复用 uplink::mediaUrlFor
// （那份实现有越界即不给的安全判据，且有离线契约覆盖）。
// ============================================================================

#include <QJsonObject>
#include <QString>

#include "alarm_manager.h"   // AlarmRecord

namespace pad {

// ============================================================================
// 时间：库里存的是定宽本地时间文本 "yyyy-MM-ddTHH:mm:ss"（无偏移）
// ============================================================================
// v1 §2.2 要求 ISO-8601 **带时区**。落库格式不能动（alarm_dao 的时间过滤靠定宽文本做
// 字典序 BETWEEN，加偏移会让 25 字符的新行与 19 字符的老行混在一起比），所以只在出门口补。
// 空串/解析失败 → 回落到"现在"，因为 updatedAt 是对端的合并与游标依据，不能为空。
QString wireTime(const QString &internalIso);
QString wireNow();

// 反向：对端游标（带偏移，甚至带毫秒）→ 库内定宽文本 "yyyy-MM-ddTHH:mm:ss"。
// 库里存的是**本地墙上时间**，所以先把对端那个瞬间换算回本机时区再落文本，
// 直接截断前 19 个字符会在"对端时钟不是 +08:00"时错 8 小时。
// 解析失败返回空串 = 调用方按"全量"处理（v1 §3.8：lastEventTime 为 null 就是全量）。
QString internalTimeFromWire(const QString &wireTimeText);

// 内部状态 → v1 三值（见文件头第 1 条）
QString wireStatus(const AlarmRecord &record);

// v1 的 readAt：已读时间。本机只有"确认"动作，没有独立已读，所以
// read_time 为空时用确认那一刻（update_time）兜底；两处都空 → 返回空串表示"未读"。
QString wireReadAt(const AlarmRecord &record);

// 所属施工计划 ID（`alarms.task_id` 列；QrLogin 的 `plan.taskId`）；未登录返回空串
QString taskIdOf(const AlarmRecord &record);

// ============================================================================
// deviceId（口径确认单 A17）
// ============================================================================
// §13.1.3 把 `deviceId` 列为 hello_ack **必填**，而 `pad_server.device_id` 出厂是空串——
// 空串的原因不是"没有 ID"，而是配置文件会被拷到别的板子，写死的 ID 会撞车。
// 所以配置留空时按本机派生一个稳定值，优先 `/etc/machine-id`（板端实测存在，克隆镜像后每台不同），
// 读不到才退到 hostname（克隆盘上 hostname 会重复，只作兜底）。**只在进程内算一次**。
QString defaultDeviceId();

// 报警类型三元组（v1 §13.2.1 要求 type/typeLabel/categoryLabel 都由边缘下发）
struct TypeWire {
    QString code;            ///< 例 HELMET / INTRUSION
    QString typeLabel;       ///< 例 未戴安全帽
    QString categoryLabel;   ///< 例 PPE违规
};
TypeWire typeOf(const AlarmRecord &record);

// ============================================================================
// 证据 URL（§3.5 / §13.2.1，2026-11-05 改版：**平铺在 alarm 顶层**）
// ============================================================================
// 本函数返回的是"要并进 alarm 顶层的那两个键"（不再有 `evidence` 对象包裹）：
//   `evidenceSnapshotUrl`：string，**必填**；取不到地址时给**空串**而不是省略键
//                          （省略=必填缺失会被整条拒收；空串=字段在、值无效，Pad 显示占位）。
//   `clipUrl`            ：string?，同名 `.mp4` 片段；没有（含片段还没落盘）给 `null`。
// baseUrl 空（媒体服务没起）或路径越出 rootDir → 绝不拼"对端取不到"的地址。
// ⚠ 这一处 2026-11-04/05 改过两轮（单键→evidence.snapshotUrls→顶层单键），原因与实测都写在
//   .cpp 里。协议边界的键名与嵌套由规约定，本机不得单方面"改得更好判空"。
// rootDir 必须是媒体服务的根（与 baseUrl 对应的那棵树），absPath 是记录里的绝对/相对路径。
QJsonObject evidenceOf(const AlarmRecord &record, const QString &baseUrl,
                       const QString &rootDir);

// §3.5 的完整快照：WS `alarm` 与 `sync_result.alarms[]` 共用同一份，别写两遍
QJsonObject alarmJson(const AlarmRecord &record, const QString &baseUrl,
                      const QString &rootDir);

// §3.6 的部分更新：status/readAt/dispose 按需给，updatedAt 必填，外加 syncState。
// 需要 baseUrl/rootDir 是为了 dispose.photoUrls——部分更新里丢了照片，别的 Pad 就永远补不上。
QJsonObject alarmUpdateJson(const AlarmRecord &record, const QString &baseUrl,
                            const QString &rootDir);

// 把上面两个对象装进 §3.2 的信封（type + ts + code + message + data）
QByteArray envelope(const QString &type, const QJsonObject &data);
// 同一条链路但要非 0 code：v1 §3.5 规定协议版本不兼容时 hello_ack 直接带 code=1001
QByteArray envelopeWithCode(const QString &type, int code, const QString &message,
                           const QJsonObject &data);
QByteArray pongEnvelope();
QByteArray errorEnvelope(int code, const QString &message, const QString &detail);

} // namespace pad

#endif // PAD_WIRE_H
