#pragma once

/**
 * @brief 进程级 FFmpeg 运行时初始化（设备 + 网络）
 * @return 初始化是否可用（以 avformat_network_init 为准）
 *
 * 内部 std::call_once 保证幂等，重复调用无副作用；须在 main() 里、任何解码/推流
 * 线程启动前调用一次，返回值可忽略（失败只会在真正开流时以 error 信号体现）。
 *
 * 为什么需要它：
 *   - avdevice_register_all()：不注册就没有 v4l2 输入源，/dev/videoN 打不开。
 *     智能识别页用 FFmpegVideoDecoder 直接开本机摄像头，此前只有
 *     CameraPreviewDecoder 自己 call_once 注册过，走不到那条路径。
 *   - avformat_network_init()：不初始化就拉不了 RTSP/RTMP，GB28181 媒体源必需。
 */
bool initializeMediaRuntime();
