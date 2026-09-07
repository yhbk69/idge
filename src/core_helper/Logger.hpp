/*
 * Logger.hpp - 异步彩色日志系统
 *
 * 特点：
 *   - 异步写入：调用线程格式化消息后入队，后台线程负责 I/O
 *   - 减少 I/O 阻塞：高频日志场景下性能提升明显
 *   - 线程安全：多线程并发调用 LOG_* 宏安全
 *   - 优雅关闭：析构时等待队列清空
 *
 * 日志级别和颜色：
 *   - DEBUG: 灰色
 *   - INFO:  绿色
 *   - WARN:  黄色
 *   - ERROR: 红色
 *   - ALARM: 红色加粗（报警专用）
 *
 * 使用示例:
 *   LOG_INFO("[Module] Message: %s", value);
 *   LOG_ERROR("[Module] Error: %d", ret);
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

// ANSI 颜色代码
namespace LogColor {
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* RED     = "\033[31m";
    constexpr const char* GREEN   = "\033[32m";
    constexpr const char* YELLOW  = "\033[33m";
    constexpr const char* BLUE    = "\033[34m";
    constexpr const char* MAGENTA = "\033[35m";
    constexpr const char* CYAN    = "\033[36m";
    constexpr const char* WHITE   = "\033[37m";
    constexpr const char* GRAY    = "\033[90m";

    constexpr const char* BOLD_RED   = "\033[1;31m";
    constexpr const char* BOLD_GREEN = "\033[1;32m";
    constexpr const char* BOLD_YELLOW = "\033[1;33m";
}

// 日志级别
enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    ALARM
};

// 全局日志级别控制
inline LogLevel& getGlobalLogLevel() {
    static LogLevel level = LogLevel::INFO;
    return level;
}

inline void setLogLevel(LogLevel level) {
    getGlobalLogLevel() = level;
}

// 异步日志队列 + 后台写入线程
class AsyncLogger {
public:
    static AsyncLogger& instance() {
        static AsyncLogger inst;
        return inst;
    }

    // 推入一条预格式化的日志消息
    void push(std::string msg) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(std::move(msg));
        }
        cv_.notify_one();
    }

    // 刷出所有待写入消息（同步）
    void flush() {
        std::vector<std::string> batch;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            batch.swap(queue_);
        }
        for (auto& m : batch)
            fprintf(stderr, "%s", m.c_str());
    }

    ~AsyncLogger() {
        running_ = false;
        cv_.notify_one();
        if (writer_.joinable())
            writer_.join();
        flush();  // 最后刷出剩余消息
    }

private:
    AsyncLogger() : running_(true) {
        writer_ = std::thread([this]() { run(); });
    }

    void run() {
        while (running_.load()) {
            std::vector<std::string> batch;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() {
                    return !queue_.empty() || !running_.load();
                });
                batch.swap(queue_);
            }
            for (auto& m : batch)
                fprintf(stderr, "%s", m.c_str());
        }
    }

    std::thread writer_;
    std::atomic<bool> running_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::vector<std::string> queue_;
};

// 内部日志函数（异步）
inline void logMessage(LogLevel level, const char* tag, const char* fmt, ...) {
    if (level < getGlobalLogLevel()) return;

    const char* color;
    const char* levelStr;

    switch (level) {
        case LogLevel::DEBUG: color = LogColor::GRAY;     levelStr = "DEBUG"; break;
        case LogLevel::INFO:  color = LogColor::GREEN;    levelStr = "INFO "; break;
        case LogLevel::WARN:  color = LogColor::YELLOW;   levelStr = "WARN "; break;
        case LogLevel::ERROR: color = LogColor::RED;      levelStr = "ERROR"; break;
        case LogLevel::ALARM: color = LogColor::BOLD_RED; levelStr = "ALARM"; break;
        default:              color = LogColor::WHITE;    levelStr = "?????"; break;
    }

    // 格式化消息体
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 构造完整日志行：颜色 + 级别 + 标签 + 消息 + 换行 + 重置
    std::string line;
    line.reserve(128);
    line.append(color);
    line.append("[");
    line.append(levelStr);
    line.append("]");
    line.append(LogColor::RESET);
    line.append(tag);
    line.append(buf);
    line.append("\n");

    AsyncLogger::instance().push(std::move(line));
}

// 便捷宏
#define LOG_DEBUG(tag, fmt, ...) logMessage(LogLevel::DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)  logMessage(LogLevel::INFO, tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)  logMessage(LogLevel::WARN, tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) logMessage(LogLevel::ERROR, tag, fmt, ##__VA_ARGS__)
#define LOG_ALARM(tag, fmt, ...) logMessage(LogLevel::ALARM, tag, fmt, ##__VA_ARGS__)

#endif // LOGGER_H
