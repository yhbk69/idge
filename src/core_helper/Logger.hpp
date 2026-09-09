/**
 * @file Logger.hpp
 * @brief 异步彩色日志系统
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

// ============================================================================
// LogColor 命名空间 —— ANSI 终端颜色代码
// ============================================================================
// 作用：定义终端输出的 ANSI 颜色转义序列，用于日志级别的彩色显示。
//       在支持 ANSI 颜色的终端中，不同级别的日志会以不同颜色显示。
// ============================================================================
namespace LogColor {
    constexpr const char* RESET   = "\033[0m";     // 重置所有属性
    constexpr const char* RED     = "\033[31m";    // 红色（ERROR）
    constexpr const char* GREEN   = "\033[32m";    // 绿色（INFO）
    constexpr const char* YELLOW  = "\033[33m";    // 黄色（WARN）
    constexpr const char* BLUE    = "\033[34m";    // 蓝色
    constexpr const char* MAGENTA = "\033[35m";    // 品红色
    constexpr const char* CYAN    = "\033[36m";    // 青色
    constexpr const char* WHITE   = "\033[37m";    // 白色
    constexpr const char* GRAY    = "\033[90m";    // 灰色（DEBUG）

    constexpr const char* BOLD_RED   = "\033[1;31m";  // 加粗红色（ALARM）
    constexpr const char* BOLD_GREEN = "\033[1;32m";  // 加粗绿色
    constexpr const char* BOLD_YELLOW = "\033[1;33m"; // 加粗黄色
}

// ============================================================================
// LogLevel 枚举 —— 日志级别
// ============================================================================
// 作用：定义日志的严重程度级别，从低到高依次为 DEBUG、INFO、WARN、ERROR、ALARM。
//       低于当前全局级别的日志不会被输出。
// ============================================================================
enum class LogLevel {
    DEBUG,  // 调试信息：最详细的日志，用于开发调试
    INFO,   // 普通信息：程序正常运行的关键状态
    WARN,   // 警告信息：潜在问题，不影响当前运行
    ERROR,  // 错误信息：发生了错误，可能影响功能
    ALARM   // 报警信息：严重错误，需要立即关注
};

// ============================================================================
// 全局日志级别控制
// ============================================================================
// 作用：通过内联函数提供全局日志级别的读写接口。
//       默认级别为 INFO，低于 INFO 的 DEBUG 日志不会输出。
// ============================================================================

/** @brief 获取全局日志级别的引用（线程局部静态变量） */
inline LogLevel& getGlobalLogLevel() {
    static LogLevel level = LogLevel::INFO;  // 默认 INFO 级别
    return level;
}

/**
 * @brief 设置全局日志级别
 * @param level 新的日志级别
 * 作用：设置后，低于该级别的日志将不会被输出
 */
inline void setLogLevel(LogLevel level) {
    getGlobalLogLevel() = level;
}

// ============================================================================
// AsyncLogger 类 —— 异步日志写入器（单例模式）
// ============================================================================
// 作用：管理一个日志消息队列和后台写入线程。
//       调用线程将格式化后的日志消息 push 到队列，后台线程从队列中取出
//       消息并写入 stderr。这种异步设计避免了高频日志场景下的 I/O 阻塞。
//
// 线程安全机制：
//   - mutex_ 保护队列的并发访问
//   - condition_variable 通知后台线程有新消息
//   - atomic<bool> running_ 控制后台线程的生命周期
//
// 生命周期：
//   - 构造时启动后台写入线程
//   - 析构时设置 running_=false，等待线程退出，刷出剩余消息
// ============================================================================
class AsyncLogger {
public:
    /**
     * @brief 获取单例实例
     * 作用：使用 Meyers' Singleton 模式，线程安全地获取唯一的 AsyncLogger 实例
     */
    static AsyncLogger& instance() {
        static AsyncLogger inst;
        return inst;
    }

    /**
     * @brief 推入一条预格式化的日志消息
     * @param msg 格式化后的完整日志行（包含颜色、级别、标签、内容）
     * 作用：将消息放入队列，并通知后台线程处理
     */
    void push(std::string msg) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(std::move(msg));  // 移动入队，避免拷贝
        }
        cv_.notify_one();  // 通知后台线程
    }

    /**
     * @brief 同步刷出所有待写入消息
     * 作用：将队列中的所有消息批量写入 stderr。
     *       用于确保所有日志在关键节点被完整输出。
     */
    void flush() {
        std::vector<std::string> batch;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            batch.swap(queue_);  // 批量取出，减少锁持有时间
        }
        for (auto& m : batch)
            fprintf(stderr, "%s", m.c_str());
    }

    /**
     * @brief 析构函数 —— 优雅关闭
     * 作用：停止后台线程并刷出剩余消息，确保不丢失日志
     */
    ~AsyncLogger() {
        running_ = false;          // 通知后台线程退出
        cv_.notify_one();          // 唤醒可能在等待的线程
        if (writer_.joinable())
            writer_.join();        // 等待后台线程退出
        flush();                   // 刷出剩余消息
    }

private:
    /**
     * @brief 私有构造函数 —— 启动后台写入线程
     * 作用：设置运行标志为 true，启动后台线程执行 run() 方法
     */
    AsyncLogger() : running_(true) {
        writer_ = std::thread([this]() { run(); });
    }

    /**
     * @brief 后台线程主循环
     * 作用：循环等待队列中有消息，批量取出并写入 stderr。
     *       当 running_ 为 false 且队列为空时退出循环。
     */
    void run() {
        while (running_.load()) {
            std::vector<std::string> batch;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                // 等待条件：队列非空或需要退出
                cv_.wait(lk, [this]() {
                    return !queue_.empty() || !running_.load();
                });
                batch.swap(queue_);  // 批量取出消息
            }
            // 写入 stderr（stderr 是行缓冲的，适合日志输出）
            for (auto& m : batch)
                fprintf(stderr, "%s", m.c_str());
        }
    }

    std::thread writer_;                // 后台写入线程
    std::atomic<bool> running_;         // 运行标志（原子变量，线程安全）
    std::mutex mtx_;                    // 互斥锁，保护队列
    std::condition_variable cv_;        // 条件变量，通知后台线程
    std::vector<std::string> queue_;    // 日志消息队列
};

// ============================================================================
// logMessage —— 内部日志格式化与异步写入函数
// ============================================================================
// 作用：根据日志级别选择对应的颜色和级别标签，使用 va_list 格式化消息体，
//       构造完整的日志行（颜色 + 级别 + 标签 + 消息 + 换行 + 重置），
//       然后通过 AsyncLogger 异步写入。
//
// @param level 日志级别
// @param tag   模块标签（如 "[VideoDecoder]"）
// @param fmt   printf 风格的格式化字符串
// @param ...   可变参数列表
// ============================================================================
inline void logMessage(LogLevel level, const char* tag, const char* fmt, ...) {
    // 检查日志级别，低于全局级别的日志直接跳过
    if (level < getGlobalLogLevel()) return;

    const char* color;
    const char* levelStr;

    // 根据日志级别选择颜色和级别字符串
    switch (level) {
        case LogLevel::DEBUG: color = LogColor::GRAY;     levelStr = "DEBUG"; break;
        case LogLevel::INFO:  color = LogColor::GREEN;    levelStr = "INFO "; break;
        case LogLevel::WARN:  color = LogColor::YELLOW;   levelStr = "WARN "; break;
        case LogLevel::ERROR: color = LogColor::RED;      levelStr = "ERROR"; break;
        case LogLevel::ALARM: color = LogColor::BOLD_RED; levelStr = "ALARM"; break;
        default:              color = LogColor::WHITE;    levelStr = "?????"; break;
    }

    // 使用 va_list 格式化消息体
    char buf[512];
    va_list args;
    va_start(args, fmt);           // 初始化可变参数列表
    vsnprintf(buf, sizeof(buf), fmt, args);  // 格式化到缓冲区
    va_end(args);                  // 清理可变参数列表

    // 构造完整日志行：颜色 + [级别] + 重置颜色 + 标签 + 消息 + 换行
    std::string line;
    line.reserve(128);             // 预分配内存，减少 realloc
    line.append(color);            // 添加颜色前缀
    line.append("[");
    line.append(levelStr);         // 添加级别标签
    line.append("]");
    line.append(LogColor::RESET);  // 重置颜色
    line.append(tag);              // 添加模块标签
    line.append(buf);              // 添加格式化后的消息内容
    line.append("\n");             // 添加换行符

    // 推入异步日志队列
    AsyncLogger::instance().push(std::move(line));
}

// ============================================================================
// 便捷日志宏 —— 简化日志调用
// ============================================================================
// 作用：提供不同级别的日志宏，自动填充模块标签和格式化参数。
//       使用 ##__VA_ARGS__ 处理可变参数为空的情况（GCC 扩展）。
// ============================================================================

/** @brief 调试级别日志宏 */
#define LOG_DEBUG(tag, fmt, ...) logMessage(LogLevel::DEBUG, tag, fmt, ##__VA_ARGS__)
/** @brief 信息级别日志宏 */
#define LOG_INFO(tag, fmt, ...)  logMessage(LogLevel::INFO, tag, fmt, ##__VA_ARGS__)
/** @brief 警告级别日志宏 */
#define LOG_WARN(tag, fmt, ...)  logMessage(LogLevel::WARN, tag, fmt, ##__VA_ARGS__)
/** @brief 错误级别日志宏 */
#define LOG_ERROR(tag, fmt, ...) logMessage(LogLevel::ERROR, tag, fmt, ##__VA_ARGS__)
/** @brief 报警级别日志宏 */
#define LOG_ALARM(tag, fmt, ...) logMessage(LogLevel::ALARM, tag, fmt, ##__VA_ARGS__)

#endif // LOGGER_H
