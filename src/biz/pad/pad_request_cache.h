#ifndef PAD_REQUEST_CACHE_H
#define PAD_REQUEST_CACHE_H

// ============================================================================
// pad_request_cache.h — v1 §2.5 的 requestId 幂等表
// ============================================================================
//
// 规约原话（plan/edge-pad-v1.md §2.5）：
//   边缘设备按 `requestId → 已返回响应` 保存至少 24 小时；同一 requestId 重复提交
//   直接返回**第一次的结果**，**不比较请求内容**，不重复创建/修改。
// 三条从这句话里必须落出来的决定，都写在这份实现的对应位置：
//   1) 命中判据只有 request_id 一个键。所以库里不存请求正文的哈希，也不做任何"内容不一致
//      就报错"的分支（规约明写"调用方不得把同一个 requestId 用于不同业务请求，
//      边缘设备不做内容冲突检测"）。
//   2) **错误响应同样进表**。"第一次的结果"包括 409/422/500 —— 只缓存成功会让重试的
//      第二个请求真的再执行一遍业务（对端超时重试时就会看到两次副作用）。
//   3) 存**序列化后的原样字节**而不是重新拼一遍信封：重放必须逐字节一致，
//      否则同一 requestId 两次拿到的 updatedAt 不同，对端会以为是两条新结果。
//
// 为什么落库而不是放内存：Pad 的超时重试完全可能跨本机一次重启（现场就是边干活边重启），
// 内存表会在最该起作用的时候失效；而这张表天然极小（按 24 小时窗口清），行数只有几十。
//
// 线程：与报警 DAO 同一口径——DatabaseManager 按线程分配连接，
// 本类的每个方法都自己取当前线程的连接，所以只能在 GUI 线程（HTTP 服务端所在线程）调用。
// ============================================================================

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <functional>

#include "pad_http_message.h"

namespace pad {

/// 一条已缓存的首次回复（够原样重放即可）
struct CachedReply {
    int status = 200;              ///< HTTP 状态码
    QString contentType;           ///< 空 = application/json
    QByteArray body;               ///< §2.4 信封原样字节
};

/**
 * @brief requestId → 首次响应 的读写（无状态，每次调用自己开当前线程的连接）
 *
 * 保留窗口 24 小时（规约下限），清理挂在 store 上：写一次顺带清一次。
 * 这张表按 24 小时窗口只有几十行，全表扫的代价远小于"多一个定时器要管生命周期"。
 */
class RequestIdCache
{
public:
    /// 保留小时数（默认 24，就是 §2.5 那句"至少 24 小时"的下限）
    explicit RequestIdCache(int retainHours = 24);

    /**
     * @brief 查这个 requestId 是否已经回过话
     * @return true = 命中，首次回复已在 out 里；false = 没命中**或库没开**
     *
     * ⚠ 库没开时返回 false 是刻意的：宁可让这一次写真的执行两遍，
     *   也不能因为幂等表读不到就把所有写接口全判失败（那等于一个库故障打穿整条处置链路）。
     */
    bool lookup(const QString &requestId, CachedReply *out) const;

    /// 记下首次回复。库没开时静默不记（同上：不因此拒绝业务），但会打一次 qWarning。
    void store(const QString &requestId, const QString &route, const padhttp::Reply &reply);

    /// 删掉超过保留窗口的行，返回删除条数（store 内部会自动调）
    int prune();

    /// 表里当前行数（诊断/自测用）
    int rowCount() const;

    /**
     * @brief §2.5 规定的 requestId 取值判据（唯一一份，别在各路由里各写一遍）
     * @param body  写接口的 JSON body
     * @param err   [out] 中文失败原因
     * @return 空串 = 不合格：缺失/空白/超过 64 字符（UUID 是 36，64 已经宽松）
     *
     * 超长要拒而不是截断：截断会把两个不同的 UUID 折叠成同一个键，
     * 于是第二次提交拿到第一次的结果——那是**静默丢写**，比回一句 400 严重得多。
     */
    static QString requireRequestId(const QJsonObject &body, QString *err);

private:
    int retainHours_;
};

/**
 * @brief 把"验 requestId → 查幂等表 → 执行 → 记表"这一整段包起来
 * @param route  形如 "POST /api/alarms/{id}/dispose"，只进幂等表做排障
 * @param body   已从请求里解出来的 JSON 对象
 * @param work   真正的业务执行，返回要发给对端的 Reply（**不要**在里面自己查表）
 * @return 要么 400/2001（requestId 不合格），要么首次回复的重放，要么 work() 的结果
 *
 * 每个写接口都走这里，是为了让"幂等"这件事只有一份实现：
 * 少调一次 store 的接口在 Pad 超时重试时就会重复执行，而那种重复在联调里极难复现。
 */
padhttp::Reply runIdempotent(const QString &route, const QJsonObject &body,
                             const std::function<padhttp::Reply()> &work);

}  // namespace pad

#endif  // PAD_REQUEST_CACHE_H
