// ============================================================================
// zlm_env.cpp — mk_env_init1 的进程级一次性门
// ============================================================================

#include "zlm_env.h"

#include <atomic>

#include "mk_common.h"

namespace {

// IO 线程数与日志级别沿用 HttpFileService 的实测口径（见 http_file_service.cpp 注释）：
// 静态文件 + rtsp 直播都是"突发/持续小流量"，2 个 IO 线程够；日志取 Warn，
// 否则每个 RTSP 客户端连接/断开各打一条会把 idge 自己的检测日志刷掉。
constexpr int kIoThreads = 2;
constexpr int kLogLevelWarn = 3;

std::atomic<bool> g_envReady{false};

}  // namespace

namespace zlm {

bool ensureEnvInit()
{
    bool expected = false;
    if (!g_envReady.compare_exchange_strong(expected, true)) {
        return true;   // 另一个线程已经调过了
    }
    // 参数顺序照 mk_common.h:100；ini/ssl 全空 = 库内默认值。
    mk_env_init1(kIoThreads, kLogLevelWarn, LOG_CONSOLE, nullptr, 0,
                 0, nullptr, 0, nullptr, nullptr);
    return true;
}

bool isEnvReady()
{
    return g_envReady.load(std::memory_order_acquire);
}

}  // namespace zlm
