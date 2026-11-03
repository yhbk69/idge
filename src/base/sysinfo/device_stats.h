#ifndef DEVICE_STATS_H
#define DEVICE_STATS_H

// ============================================================================
// device_stats.h — 设备运行指标采样（Pad 接口 v1 §7.1 的那张表）
// ============================================================================
//
// 为什么放在 base：这些数原来只在 src/ui/widgets/dashboard_widget.cpp 里读，
// 而分层是 base ← media ← ai ← biz ← ui —— 网络层（biz/pad）不能 include ui 头，
// 于是同一份 /proc 读取要有第二处。这里就是那一处的唯一-owner。
// 本文件只依赖 Qt Core 与 procfs/sysfs，可离线自测（tests/…）。
//
// 三条实测口径（板端 RK3588 A88 + Ubuntu 24.04.2，uid 1000，2026-11-02 逐条核过）：
//   1) **NPU 使用率没有可信来源**，所以默认**不给值**：
//      /sys/class/devfreq/fdab0000.npu/load 可读但空闲时恒为 `100@1000000000Hz`
//      （连采 6 次、无任何推理进程在跑，仍是 100）→ 它是 devfreq 的 busy 指示，
//      不是"核上有多少活"，拿它当 npu_usage 等于对外报一个假数。
//      真节点在 /sys/kernel/debug/rknpu/load（按核百分比），但 debugfs 对 uid 1000
//      是"权限不够"，且这台内核没挂出来。→ 除非以 root 跑并确认 debugfs 可见，
//      否则 hasNpu 恒 false，调用方**省略该字段**（v1 §7.1 白纸黑字"只返回实际字段"）。
//   2) **电池是有节点的**（与"这板子没电池"的直觉相反）：/sys/class/power_supply/bat
//      给出 type=Battery、present=1、capacity、status，另有 bq25700-charger(USB, Charging)。
//      现场到底装没装电芯看不出来（本次读回 capacity=100 / status=Full），
//      所以只透传节点值，不做"没装电池就报 0"这种推断。
//   3) 温度取 `type == soc-thermal` 的那个 zone，**不是固定 thermal_zone0**：
//      本机 zone0 恰好是 soc-thermal，但 zone7 是 `bat`(25.0℃)，
//      按下标取会随机核到哪个就报哪个。
//
// CPU 占用需要两次 /proc/stat 做差，所以采样器是**有状态**的：
// 第一次 sample() 只建基线、hasCpu=false。对外服务前请调一次 seed()。
// ============================================================================

#include <QtGlobal>
#include <QString>

#include <cstdint>

namespace sysinfo {

// 一项指标要么"有值"要么"没来源"，没来源时调用方必须省略字段而不是填 0。
struct DeviceStats {
    bool    hasCpu = false;
    double  cpuUsagePercent = 0.0;      ///< 0–100，自上次采样以来的均值

    bool    hasMemory = false;
    qint64  memTotalMb = 0;
    qint64  memUsedMb = 0;              ///< total - MemAvailable（与看板同一口径）

    bool    hasBattery = false;
    int     batteryPercent = 0;         ///< 0–100，来自 power_supply/bat/capacity
    bool    batteryCharging = false;    ///< status == "Charging"

    bool    hasTemperature = false;
    double  temperatureC = 0.0;         ///< soc-thermal 区，毫度换算而来

    bool    hasNpu = false;
    double  npuUsagePercent = 0.0;      ///< 仅在 debugfs 的 rknpu/load 可读时才有值（见文件头第 1 条）

    bool    hasDisk = false;
    double  diskTotalGb = 0.0;
    double  diskUsedGb = 0.0;

    bool    hasUptime = false;
    qint64  uptimeSec = 0;

    QString systemVersion;              ///< /etc/os-release 的 PRETTY_NAME，空 = 没读到
};

// 有状态的采样器。每个对外服务持有一个实例即可（非线程安全：同一实例只在一只线程里调）。
class DeviceSampler {
public:
    // dataPath：磁盘用量按这条路径所在分区统计。空 → 回落 RuntimePaths 的报警目录，
    // 再不行用 "/"（报警媒体真正占的是那个分区）。
    explicit DeviceSampler(QString dataPath = QString());

    // 建基线：读一次 /proc/stat 但不产出百分比。服务端在启动时调一次，
    // 之后第一次对外采样就有 CPU 值（否则首个请求永远缺这个字段）。
    void seed();

    // 采一轮。CPU 与上一次调用做差，其余项每次现读。
    DeviceStats sample();

    // 磁盘统计路径（构造参数解析后的绝对路径根）。
    const QString &dataPath() const { return dataPath_; }

private:
    qint64 prevIdleMs_ = -1;    // /proc/stat 的 idle+iowait 累计（毫秒）
    qint64 prevTotalMs_ = -1;   // /proc/stat 全部字段累计（毫秒）
    QString dataPath_;
};

// ============================================================================
// 单项取值：独立函数便于逐条自测，也让上面的三条实测口径有各自的落点
// ============================================================================
// /proc/stat 首行的累计时间（单位换算成毫秒：Hz=100 → 1 tick = 10ms）。false = 读不到。
bool readCpuTicks(qint64 *idleMs, qint64 *totalMs);
// 两次快照求百分比。时间没往前走（首次调用、或 procfs 异常）返回 false。
bool cpuPercentFromTicks(qint64 prevIdleMs, qint64 prevTotalMs,
                         qint64 idleMs, qint64 totalMs, double *outPercent);
qint64 memTotalMb();
qint64 memUsedMb();      ///< total - MemAvailable
bool   readBattery(int *percent, bool *charging);
bool   readSocTempC(double *outC);
bool   readNpuLoadPercent(double *outPercent);   ///< 见文件头第 1 条：本机恒 false
bool   readUptimeSec(qint64 *outSec);
QString osPrettyName();

} // namespace sysinfo

#endif // DEVICE_STATS_H
