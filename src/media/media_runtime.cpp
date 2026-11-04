/**
 * @file media_runtime.cpp
 * @brief media 域 —— 进程级 FFmpeg 运行时初始化的实现
 *
 * 所属模块：src/media（media 根，不在 reader/rga/server 任一子目录）。
 * 核心职责：把"必须先于任何开流动作执行一次"的两条 FFmpeg 全局初始化收成一个
 *           幂等入口，供 main() 调用；声明与"为什么需要它"见 media_runtime.h。
 *
 * 为什么单独一个编译单元：avdevice_register_all() / avformat_network_init() 都是
 * 进程级全局副作用，散在各解码器里就会出现"谁先跑谁负责"的隐式顺序依赖
 * （历史上 v4l2 注册只藏在 CameraPreviewDecoder 的 call_once 里，
 * 智能识别改用 FFmpegVideoDecoder 开本机摄像头时就打不开了）。
 */
#include "media_runtime.h"
#include <mutex>
extern "C" {
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
}

/**
 * @brief 执行（且只执行一次）设备 + 网络初始化
 * @return 本次进程是否完成了可用的初始化；false = avformat_network_init 失败
 * @note 线程安全：std::call_once 保证只跑一遍，多线程并发调用不会重复注册。
 * @note ⚠ 失败是**粘性**的：ready 是函数级 static，一旦为 false，后续调用不会
 *       重试、也永远拿不到 true。avdevice_register_all() 无返回值、本身不报错，
 *       所以这里唯一的失败来源是 avformat_network_init（内存分配失败一类），
 *       实际极罕见；调用方按头文件约定忽略返回值即可，不要在这里"顺手加重试"。
 * @note 与之对称的清理（avformat_network_deinit）本工程**没有做**：进程退出即回收。
 */
bool initializeMediaRuntime() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        avdevice_register_all();
        ready = avformat_network_init() >= 0;
    });
    return ready;
}
