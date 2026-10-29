#ifndef MEDIA_PUMP_H
#define MEDIA_PUMP_H

/**
 * @file media_pump.h
 * @brief 报警媒体的后台读盘器（进程内一条工作线程 + 有界队列）
 *
 * 与 SnapWriter / ClipWriter 同一套范式：提交方立即返回，等文件、读全量、算 sha256
 * 全在这条线程上，成品再投回 GUI 线程交给 AlarmUplinkEngine 切块发送。
 *
 * 为什么绝不能在 GUI 线程读盘：一次报警要读约 365KB 的抓拍图加约 1.2MB 的片段，
 * 还要各算一遍 SHA-256，而且"等文件写稳"本身要 sleep。放在 GUI 线程就是直接把
 * 「4 路 1080p 零卡顿」的成果砸掉；放在解码线程则会拖慢推理节拍。
 *
 * 为什么成品要投回 GUI 而不是这条线程直接发：WsClient 只在 GUI 线程（见 ws_client.h
 * 的线程模型），socket 只能在它所属的线程里写。这条线程只做"字节准备"。
 */

#include <QObject>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "media_payload.h"

namespace uplink {

class MediaPump : public QObject
{
    Q_OBJECT

public:
    struct Request {
        QString mediaId;              ///< 与 alarm_info 里预告的那个一致
        QString alarmId;              ///< 归属报警
        QString kind;                 ///< snapshot | clip
        QString path;                 ///< 本机落盘路径
        QString mime;                 ///< 兜底 MIME（真实后缀换算优先）
        /// 结果投回哪个对象（必须是 GUI 线程的对象）。空 = 丢弃结果。
        /// 用 context 而不直接调回调，是为了让"对象已析构"时事件被 Qt 自动丢掉，
        /// 不会回调到一个悬垂的 this 上
        QObject *context = nullptr;
        /// 载荷按 const 引用给：MediaPayload 里唯一的bulk 是 QByteArray（引用计数），
        /// 用 && 传反而要求回调那侧的 lambda 是 mutable 的，没有收益
        std::function<void(const MediaPayload &payload, const QString &error)> onDone;
    };

    /// 首次调用时启动工作线程（与 ClipWriter::get() 同法，不用时不占线程）
    static MediaPump &get();

    /**
     * @brief 提交一个读盘任务，立即返回
     * @return 入队后的队列深度（>1 说明这条线程已经排活，可用于反压观测）
     * @note 队列满丢最旧：积压时队首那张图/那段视频对应的报警早就过去了，
     *       留新的更有价值（与 SnapWriter/ClipWriter 同一口径）
     */
    int submit(Request request);

    /// 停线程并丢掉排队中的任务（进程退出路径；析构里也会调）
    void shutdown();

    int queueDepth();

private:
    MediaPump();
    ~MediaPump() override;
    Q_DISABLE_COPY(MediaPump)

    void run();

    /// 把被挤掉的那个任务按"失败"投回它的 context（见 submit 里的说明）
    static void reportDropped(Request &dropped);

    /// 每条报警最多产出 1 张图 + 1 段视频，6 项 = 3 次报警的余量
    static constexpr int kMaxPending = 6;
    /// 等文件写稳的上限：截图线程正常时几十毫秒就到位，4 秒还没稳就是真出问题了
    static constexpr int kFileWaitTimeoutMs = 4000;

    std::thread th_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool stop_ = false;
};

}  // namespace uplink

#endif  // MEDIA_PUMP_H
