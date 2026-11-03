# src/base/sysinfo

> 所属域：**base 基础设施域**（本轮新增）

## 功能概述
设备运行指标的**唯一**采样点：CPU / 内存 / SoC 温度 / 电池 / NPU / 磁盘 / uptime / 系统版本。
加这一层的理由是分层——看板（`src/ui/widgets/dashboard_widget.cpp`）已经在读 `/proc`，
而 Pad 接口 v1 的 `GET /api/device/status`（§7.1）要同一批数；网络层在 `biz`，
按 `base ← media ← ai ← biz ← ui` 不能回头 include `ui` 的头。
没有这一层，"两处各读一遍、口径还不一样"就是必然结果。

**当前还没有消费者**：v1 的 HTTP 面本轮未实现（§7.1 依赖 `/api/` 那一层）。
本模块是它的前置，加上看板可以逐步换过来。采样器本身有板端实测，换 UI 是纯搬运。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `device_stats.h` / `.cpp` | `sysinfo::DeviceStats`（每项带 `has*` 标志）+ `DeviceSampler`（有状态，CPU 要两次快照做差）+ 七个单项函数 |

## 三条实测口径（RK3588 A88 + Ubuntu 24.04.2，uid 1000，2026-11-02 逐条核过）

| 项 | 结论 | 证据 |
| --- | --- | --- |
| **NPU 使用率给不出来** | `hasNpu` 恒 false，调用方**省略字段** | `/sys/class/devfreq/fdab0000.npu/load` 可读，但**空闲时恒为 `100@1000000000Hz`**（连采 6 次、机上无任何推理进程）——它是 devfreq 的 busy 指示，不是"核上有多少活"，拿它当 `npu_usage` 等于对外报假数。真节点在 `/sys/kernel/debug/rknpu/load`，而 debugfs 对 uid 1000 是"权限不够"。看板那行"3 核在线"本来就标了非实测（`dashboard_widget.cpp:358-360`） |
| **电池是有节点的** | `hasBattery` 可以给值 | `/sys/class/power_supply/bat` → `type=Battery`、`present=1`、`capacity=100`、`status=Full`，另有 `bq25700-charger`(USB, Charging)。与"这块板子是固定供电、没有电池"的直觉相反，所以只透传节点值、不做"没装电芯就报 0"的推断 |
| **温度按 type 找** | 取 `type == soc-thermal` 的 zone | 本机 8 个 zone 里 `thermal_zone0` 是 soc-thermal，`thermal_zone7` 是 `bat`(25.0℃)——按下标取会随机核到电池区。**另外坑在枚举**：`thermal_zone*` 是符号链接，`QDir::Dirs \| QDir::NoSymLinks` 列回来是空表（实测 filter=9 → 空、filter=24583 → 8 个全在），必须用 `AllEntries`，否则温度项静默变成"没来源" |

## 用法
```cpp
sysinfo::DeviceSampler sampler;     // 一个对外服务持有一个（非线程安全）
sampler.seed();                     // 建 CPU 基线：不 seed 的话首个请求永远缺 cpu_usage
...                                 // 之后每次要数就 sample()，CPU 是"自上次以来"的均值
const sysinfo::DeviceStats st = sampler.sample();
if (st.hasCpu) { /* 填 cpu_usage */ }   // 没来源的一律**省略字段**，不填 0
```
`disk` 按构造参数那条路径所在分区统计；空则用 `RuntimePaths::alarmsDir()` 绝对化后的分区
（报警媒体真正吃的是那块盘），再退到 `/`。

## 实测输出（本机，板端跑 `device_stats.cpp` + 一段 main 直接打印）
```
cpu=13.3%(has=1) mem=8266/15952MB battery=100%(chg=0,has=1) temp=49.9C(has=1)
npu=0.0(has=0) disk=108.8/231.5GB uptime=551914s os=Ubuntu 24.04.2 LTS
```

## 依赖方向
只用 `Qt5::Core`（`QFile`/`QDir`/`QStorageInfo`/`QRegularExpression`）+ `unistd.h` +
`src/base/runtime_paths.h`。无 GUI、无业务依赖，可在任意 Linux 上跑；
`/proc` 读不到时返回 `has*=false`，不猜值。
