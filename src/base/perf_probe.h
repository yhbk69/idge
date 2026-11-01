// 文件职责：性能探针开关。GUI 卡顿排查用的计时日志统一由它控制，默认关闭。
//
// 【为什么要有这个】RK3588 上"画面卡顿"类问题只能靠真机插桩定位：在可疑函数前后
// 打 QElapsedTimer，把耗时和 paintGL 间隔对到同一条时间轴上才能确认因果。这些探针
// 长期留在代码里比每次临时加了再删更省事，但无条件输出会反过来污染测量
// （qWarning 走 stderr，一次报警刷新曾刷出上千行），所以默认零开销关闭。
//
// 【用法】IDGE_PERF=1 ./idge 开启；配合
//   QT_MESSAGE_PATTERN='[%{time hh:mm:ss.zzz}] %{type} %{message}'
// 才有时间戳可对齐（Qt 默认不带）。
//
// 【为什么用函数内 static】qEnvironmentVariableIsSet 只在首次调用时读一次环境，
// 之后进程内恒定；写成 header 里的 namespace static 变量会每个编译单元一份，
// 且会在 main 之前参与静态初始化——避免这两个坑。
#pragma once

#include <QtGlobal>

// 探针是否开启。用在计时块已经跑起来、只在超阈值时决定"要不要打印"的位置。
inline bool perfProbe()
{
    static const bool enabled = qEnvironmentVariableIsSet("IDGE_PERF");
    return enabled;
}

// 跳过 CPU 叠加绘制（框+文字）的对照开关：IDGE_NODRAW=1。
// 用途是把"画框"整段从帧预算里摘出来做 A/B，量它对吞吐的真实占比
// （分阶段探针报的 draw 占比需要这样一次反证，而不是只看数字推断）。
//
// 板端 A/B 实测（4 路 1080p/30fps，同一份 assets/test/192.mp4）——注意这组数是
// a2c6227 之前取的，当时叠加绘制还在非缓存映射上做逐像素读-改-写，是瓶颈本体：
//   开：单帧 total 55~69ms，实际 14.5~18fps，进程 CPU 420%，节拍 sleep 仅 12us
//   关：单帧 total 33.3ms（贴着 30fps 锚线），4 路稳定 30.00fps，
//       paints/s = arrivals/s = 30.0，进程 CPU 96~103%
// 即叠加绘制一项当时吃掉约 3.2 个 A76 核。
// 987e9a6 把绘制改成整行纯写之后这一档差距已经不复存在（draw 段实测
// 1.43~1.48ms/帧，见 ffmpeg_video_decoder.cpp 里 DecodeStages 上方的本轮数字），
// 本轮没有重跑 IDGE_NODRAW 的对照，上面两行只在复现历史问题时按历史值参考。
inline bool noOverlayDraw()
{
    static const bool on = qEnvironmentVariableIsSet("IDGE_NODRAW");
    return on;
}
