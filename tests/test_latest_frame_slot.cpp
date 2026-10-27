#include "latest_frame_slot.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

int main() try {
    LatestFrameSlot<int> slot;
    std::atomic<bool> running{true};
    uint64_t fast = 0, slow = 0;
    auto first = std::make_shared<int>(1);
    std::weak_ptr<int> firstLifetime = first;
    check(slot.publish(first), "initial publish failed");
    first.reset();
    auto fastFrame = slot.waitNext(fast, running);
    auto slowFrame = slot.waitNext(slow, running);
    check(fastFrame == slowFrame && *fastFrame == 1, "subscribers did not share input");
    check(slot.publish(std::make_shared<int>(2)), "publish 2 failed");
    auto fastNext = slot.waitNext(fast, running);
    check(*fastNext == 2, "fast subscriber missed new generation");
    check(slot.publish(std::make_shared<int>(3)), "publish 3 failed");
    auto slowNext = slot.waitNext(slow, running);
    check(*slowNext == 3, "slow subscriber did not skip superseded input");
    check(!firstLifetime.expired(), "held input was recycled during inference");
    fastFrame.reset(); slowFrame.reset();
    check(firstLifetime.expired(), "superseded input leaked after readers released it");
    auto waiting = std::async(std::launch::async, [&] { return slot.waitNext(slow, running); });
    const bool repeated = waiting.wait_for(50ms) != std::future_status::timeout;
    slot.close();
    check(!repeated, "subscriber reprocessed the same generation without new input");
    check(waiting.wait_for(1s) == std::future_status::ready && !waiting.get(), "close did not wake waiter");
    check(!slot.publish(std::make_shared<int>(4)), "closed slot accepted work");
    slot.reset(); fast = 0;
    check(slot.publish(std::make_shared<int>(5)), "restart publish failed");
    check(*slot.waitNext(fast, running) == 5, "restart retained stale generation");
    slot.close();
    std::cout << "latest-frame broadcast/skip/lifetime/stop/restart contracts passed\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
