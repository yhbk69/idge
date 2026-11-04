// ============================================================================
// media_pump.cpp — 报警媒体后台读盘线程
// ----------------------------------------------------------------------------
// 结构照 ClipWriter：一条 std::thread + mutex/cv + 有界 deque，满了丢最旧。
// 与 ClipWriter 的差别只有两件事：
//   1) 没有"延迟到点"这一维——片段任务在 ClipWriter 落盘之后才提交过来，图也是异步写完的，
//      这里要等的是"文件写稳"（loadMediaPayload 内部按 size 采样判断），不是墙钟时刻；
//   2) 产出不是文件而是内存里的字节 + sha256，成品必须投回 GUI 线程才能发（socket 的线程约束）。
// 提交方（GUI）与这条线程之间只通过 queue_ 交接，Request 里的回调只在 context 所在线程执行。
// ============================================================================

#include "media_pump.h"

#include <QMetaObject>

namespace uplink {

MediaPump &MediaPump::get()
{
    static MediaPump pump;      // 首次提交时启动（与 ClipWriter::get() 同法）
    return pump;
}

MediaPump::MediaPump()
{
    th_ = std::thread([this] { run(); });
}

MediaPump::~MediaPump()
{
    shutdown();
}

void MediaPump::shutdown()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) {
            return;             // 重复调不再 join
        }
        stop_ = true;
        queue_.clear();         // 退出时不再读盘：没人收的文件读了也是白读
    }
    cv_.notify_all();
    if (th_.joinable()) {
        th_.join();
    }
}

int MediaPump::submit(Request request)
{
    Request evicted;
    bool hasEvicted = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stop_) {
            return 0;
        }
        if (static_cast<int>(queue_.size()) >= kMaxPending) {
            evicted = std::move(queue_.front());   // 满了丢最旧，与 SnapWriter/ClipWriter 同口径
            queue_.pop_front();
            hasEvicted = true;
        }
        queue_.push_back(std::move(request));
        cv_.notify_one();
    }
    if (hasEvicted) {
        reportDropped(evicted);
    }
    return static_cast<int>(queue_.size());
}

// 被挤掉的一方必须知道：alarm_info 已经把那个 media_id 承诺给对端了，静默丢
// 就变成对端无限等一段永远不来的字节。走同一条投递路径回 GUI 线程，回调里
// 只会看到 error 非空 + 空载荷
// NOTE(线程安全): submit() 末尾 `return queue_.size()` 是在锁外读队列，与 run() 线程的
// pop_front 并发。该返回值只可用于观测积压趋势，别拿它当判决条件；要精确深度请调
// queueDepth()（那个加了锁）。
void MediaPump::reportDropped(Request &dropped)
{
    QObject *context = dropped.context;
    auto callback = std::move(dropped.onDone);
    if (!context || !callback) {
        return;
    }
    MediaPayload empty;
    const QString error = QStringLiteral("读盘队列已满，%1(%2) 被更晚的媒体挤掉")
                              .arg(dropped.kind, dropped.mediaId);
    QMetaObject::invokeMethod(
        context,
        [callback = std::move(callback), empty = std::move(empty), error]() {
            callback(empty, error);
        },
        Qt::QueuedConnection);
}

int MediaPump::queueDepth()
{
    std::lock_guard<std::mutex> lk(mtx_);
    return static_cast<int>(queue_.size());
}

void MediaPump::run()
{
    for (;;) {
        Request req;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
            if (stop_) {
                return;
            }
            req = std::move(queue_.front());
            queue_.pop_front();
        }

        // 等文件、读全量、算哈希——这条线程存在的唯一理由就是让这三步离 GUI 线程远点
        MediaPayload payload;
        QString error;
        const bool ok = loadMediaPayload(req.mediaId, req.alarmId, req.kind, req.path, req.mime,
                                         kFileWaitTimeoutMs, &payload, &error);
        if (!ok) {
            payload = MediaPayload{};   // 失败就交空载荷，回调只看 error
        }

        QObject *context = req.context;
        auto callback = std::move(req.onDone);
        if (!context || !callback) {
            continue;
        }
        // 投回 context 所在线程（GUI）：只有那里允许碰 AlarmUplinkEngine 和 socket。
        // context 已析构时 Qt 会丢掉这个事件，回调不会打到悬垂指针上
        QMetaObject::invokeMethod(
            context,
            [callback = std::move(callback), payload = std::move(payload), error]() {
                callback(payload, error);
            },
            Qt::QueuedConnection);
    }
}

}  // namespace uplink
