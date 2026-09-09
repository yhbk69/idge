#pragma once
// ============================================================================
// BlockingQueue - 有界阻塞队列
// ============================================================================
//
// 作用：
//   在多线程 Pipeline 中实现线程安全的帧/包传递。
//   当队列满时，push 会阻塞（或丢帧）；当队列空时，pop 会阻塞。
//
// 使用场景：
//   - 解码线程 → 推理线程：传递解码后的帧
//   - 推理线程 → 结果队列：传递检测结果
//   - 编码线程 → 推流线程：传递编码后的帧
//
// 设计特点：
//   - 有界队列：防止内存无限增长
//   - 背压机制：队列满时生产者被阻塞，避免堆积
//   - 非阻塞模式：timeout_ms=0 时立即返回，用于可丢帧场景
//   - 最新帧语义：push_latest 丢弃旧帧，保证处理最新数据
//   - 优雅关闭：close() 唤醒所有阻塞线程
//
// ============================================================================

#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>

// 有界阻塞队列，用于 Pipeline 各线程之间的帧/包传递。
//
// 容量上限由构造参数 max_size 决定，各队列的容量设计：
//   enc_queue(1)    — 采集→编码，容量=1 形成背压，防止采集过快堆积
//   infer_queue(2)  — 采集→推理，push 用 timeout=0（非阻塞），推理忙时直接丢帧
//   stream_queue(2) — 编码→推流，容量=2 平滑编码抖动（~137ms 缓冲）
//   mqtt_queue(16)  — 推理→MQTT，容量大避免 MQTT 网络延迟反压推理线程
//
// close() 后所有阻塞的 push/pop 立即返回 false，线程可据此退出循环。
template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t max_size) : max_size_(max_size), closed_(false) {}

    // ============================================================================
    // push: 向队列投递一项
    // ============================================================================
    // 队列满时的行为由 timeout_ms 决定：
    //   timeout_ms < 0  → 永久阻塞等待空位（用于不能丢帧的场景）
    //   timeout_ms = 0  → 立即返回 false（非阻塞，用于可丢帧场景）
    //   timeout_ms > 0  → 等待至多 timeout_ms 毫秒
    //
    // 队列已关闭时始终返回 false。
    //
    // 线程安全：
    //   - 使用 mutex 保护队列操作
    //   - 使用条件变量实现阻塞等待
    // ============================================================================
    bool push(T item, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待条件：队列未满 或 队列已关闭
        auto pred = [this] { return queue_.size() < max_size_ || closed_; };
        if (timeout_ms < 0) {
            // 永久阻塞等待
            not_full_.wait(lock, pred);
        } else {
            // 带超时的等待
            if (!not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred))
                return false;  // 超时
        }
        if (closed_) return false;  // 队列已关闭
        queue_.push(std::move(item));
        not_empty_.notify_one();  // 唤醒等待 pop 的线程
        return true;
    }

    // ============================================================================
    // push_latest: 最新帧语义
    // ============================================================================
    // 队列满时淘汰尚未消费的旧项，再写入新项。
    // 用于采集→推理链路，保证推理开始时拿到的永远是最近画面，而不是最早积压画面。
    //
    // 为什么需要这个？：
    //   - 视频流是实时的，旧帧没有价值
    //   - 如果用普通 push，队列满时会阻塞，导致延迟累积
    //   - push_latest 丢弃旧帧，保证始终处理最新数据
    //
    // 参数 replaced：如果非空，记录被丢弃的帧数
    // ============================================================================
    bool push_latest(T item, size_t* replaced = nullptr) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) return false;
        // 如果队列满，丢弃最旧的一帧
        if (queue_.size() >= max_size_) {
            queue_.pop();
            if (replaced) ++*replaced;
        }
        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one();  // 唤醒等待 pop 的线程
        return true;
    }

    // ============================================================================
    // pop: 从队列取出一项
    // ============================================================================
    // 队列空时的行为由 timeout_ms 决定：
    //   timeout_ms < 0  → 永久阻塞等待数据
    //   timeout_ms = 0  → 立即返回 false
    //   timeout_ms > 0  → 等待至多 timeout_ms 毫秒
    //
    // 各线程 run() 循环通常用 200ms 超时，超时后检查 running_ 标志，
    // 避免线程永久卡死。
    //
    // 队列已关闭且为空时返回 false，线程可据此退出。
    // ============================================================================
    bool pop(T& item, int timeout_ms = -1) {
        std::unique_lock<std::mutex> lock(mutex_);
        // 等待条件：队列非空 或 队列已关闭
        auto pred = [this] { return !queue_.empty() || closed_; };
        if (timeout_ms < 0) {
            // 永久阻塞等待
            not_empty_.wait(lock, pred);
        } else {
            // 带超时的等待
            if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred))
                return false;  // 超时
        }
        if (queue_.empty()) return false;  // 队列已关闭且为空
        item = std::move(queue_.front());
        queue_.pop();
        not_full_.notify_one();  // 唤醒等待 push 的线程
        return true;
    }

    // ============================================================================
    // close: 关闭队列
    // ============================================================================
    // 唤醒所有阻塞的 push/pop，使其返回 false。
    // main() 在停止上游线程后调用，通知下游线程数据流已结束。
    //
    // 使用场景：
    //   - 用户点击"停止"按钮
    //   - 视频流结束
    //   - 程序退出
    // ============================================================================
    void close() {
        std::unique_lock<std::mutex> lock(mutex_);
        closed_ = true;
        // 唤醒所有等待的线程
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T>           queue_;           // 底层队列
    mutable std::mutex      mutex_;           // 互斥锁（保护队列操作）
    std::condition_variable not_empty_;       // 非空条件变量（唤醒 pop）
    std::condition_variable not_full_;        // 未满条件变量（唤醒 push）
    size_t max_size_;                         // 队列最大容量
    bool   closed_;                           // 是否已关闭
};
