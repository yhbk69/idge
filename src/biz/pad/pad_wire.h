#ifndef PAD_WIRE_H
#define PAD_WIRE_H

// ============================================================================
// pad_wire.h — AlarmRecord ↔ Pad 接口 v1 的 `alarm` / `alarm_update` JSON
// ============================================================================
//
// 为什么单独一个文件：这份映射是**协议边界**，散到 WS 服务端、HTTP 层、UI 里就会变成三处，
// 而 v1 要求同一 alarmId 的合并只看 updatedAt（§3.8），三处口径不一致对端就会时而看到
// PENDING 时而看到 DISPOSED。所有出站报警 JSON 只能从这里出。
//
// 三条口径要先看清（都是"规约没写死、这里必须选一个"的地方，理由写在 .cpp 对应函数上）：
//   1) 本机 `rectified`（已确认）**不等于** v1 的 `DISPOSED`（已处置）。
//      v1 只认 PENDING/DISPOSED/FALSE_ALARM（§3.10），而本机的"确认"语义是"我看过了"
//      → 映射成 **PENDING + readAt**，处置信息一概不臆造。
//   2) `taskId` 只有 `construction_id` 这一个来源（扫码登录定死，见 B2）。
//      未登录期间产生的报警 `taskId` 只能是空串：它不属于任何施工计划，
//      对端按 taskId 过滤时看不到它——这是事实，不是缺陷。
//   3) v1 的 `alarm_update` **不带 evidence**（§13.2.3 只有 status/readAt/dispose/updatedAt），
//      而本机报警那一刻视频片段还没落盘（ClipWriter 是"报警后 2 秒"才写）。
//      → 片段到位后**再发一条完整的 `alarm`**（§3.8 明说重复下发同一报警是安全的、按 updatedAt 合并），
//        而不是发一条带不上证据的 update。
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

// 所属施工计划 ID（construction_info.construction_id）；未登录返回空串
QString taskIdOf(const AlarmRecord &record);

// 报警类型三元组（v1 §13.2.1 要求 type/typeLabel/categoryLabel 都由边缘下发）
struct TypeWire {
    QString code;            ///< 例 HELMET / INTRUSION
    QString typeLabel;       ///< 例 未戴安全帽
    QString categoryLabel;   ///< 例 PPE违规
};
TypeWire typeOf(const AlarmRecord &record);

// ============================================================================
// 证据 URL
// ============================================================================
// baseUrl 空（媒体服务没起）→ 整个 evidence 只有空数组，不臆造地址。
// rootDir 必须是媒体服务的根（与 baseUrl 对应的那棵树），absPath 是记录里的绝对/相对路径。
QJsonObject evidenceOf(const AlarmRecord &record, const QString &baseUrl,
                       const QString &rootDir);

// §3.6 的完整快照：WS `alarm` 与 `sync_result.alarms[]` 共用同一份，别写两遍
QJsonObject alarmJson(const AlarmRecord &record, const QString &baseUrl,
                      const QString &rootDir);

// §3.7 的部分更新：只带 status/readAt/dispose/updatedAt（v1 不接受更多）
QJsonObject alarmUpdateJson(const AlarmRecord &record);

// 把上面两个对象装进 §3.2 的信封（type + ts + code + message + data）
QByteArray envelope(const QString &type, const QJsonObject &data);
// 同一条链路但要非 0 code：v1 §3.5 规定协议版本不兼容时 hello_ack 直接带 code=1001
QByteArray envelopeWithCode(const QString &type, int code, const QString &message,
                           const QJsonObject &data);
QByteArray pongEnvelope();
QByteArray errorEnvelope(int code, const QString &message, const QString &detail);

} // namespace pad

#endif // PAD_WIRE_H
