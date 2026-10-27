#pragma once

/**
 * @file latest_frame_slot.h
 * @brief 单槽"最新值"广播邮箱：生产者覆盖、消费者按代跳帧
 *
 * 用途是给"只关心最新一帧、旧帧可以直接丢"的下游（预览、QR/OCR 取帧、推流）
 * 提供一个无锁排队、不会积压的交接点，取代"每帧入队 + 队列涨到几千"的写法。
 *
 * 语义（tests/test_latest_frame_slot.cpp 逐条自检）：
 *   - publish() 覆盖槽内值并递增 generation_，永不阻塞、永不排队；
 *     已有消费者持有上一帧的 shared_ptr 时，那帧不会被提前回收。
 *   - waitNext() 只在新 generation 上返回，落后的消费者一次唤醒直接拿到最新帧，
 *     中间被覆盖的帧不再重复处理（每个消费者自带游标 previous）。
 *   - close() 唤醒所有等待者并返回空指针，publish() 此后一律失败；
 *     reset() 清空并重新开放，generation_ 归零，消费者须把游标一起重置。
 *
 * 取舍：publish() 用 try_lock，抢不到锁就丢弃本帧（返回 false）而不是等待——
 * 对"最新帧"语义而言丢弃是正确行为，但也意味着 closed_ 的读取依赖同一把锁，
 * 不能改成无锁 atomic。多个消费者共享同一个 generation_ 游标会漏帧，
 * 每个消费者必须各持一份 previous。
 *
 * 现状：业务链路尚未接入，仅有单元测试覆盖，改动前先跑 test_latest_frame_slot。
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

template<class T> class LatestFrameSlot final {
public:
    /// 覆盖槽内最新值。锁被占或已 close 时丢弃本帧并返回 false（不阻塞、不排队）。
    bool publish(std::shared_ptr<T> value) {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || closed_) return false;
        value_ = std::move(value);
        ++generation_;
        lock.unlock();
        cv_.notify_all();
        return true;
    }

    /**
     * @brief 阻塞直到出现比 previous 更新的一帧，返回该帧并把 previous 推进
     *
     * previous 是**调用方自己的**游标，多个消费者各传各的，互不影响。
     * 已 close 或 running 转 false 时返回空 shared_ptr。
     */
    std::shared_ptr<T> waitNext(uint64_t& previous, const std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return closed_ || !running || generation_ != previous; });
        if (closed_ || !running) return {};
        previous = generation_;
        return value_;
    }

    /// 清空槽并把 generation_ 归零、重新开放；调用方须同时把自己的游标归零。
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        value_.reset(); generation_ = 0; closed_ = false;
    }

    /// 单向关闭：唤醒所有等待者（返回空指针），此后 publish 一律失败。
    void close() {
        { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; value_.reset(); }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<T> value_;
    uint64_t generation_ = 0;
    bool closed_ = false;
};
