# src/base/config

> 所属域：**base 基础设施域**（目录重组 S4a 迁入，依赖方向 base ← media/ai/biz/ui）

## 功能概述
配置模块。分两部分：
- **ConfigManager**（Qt/JSON）：`data/config.json` 的统一读写入口（路径常量见 `src/base/runtime_paths.h`），供 GUI/检测/报警/围栏共享。
- **ConfigParser**（CLI）：基于 `getopt_long` 的命令行参数解析，产出 `AppConfig`。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `ConfigManager.h/.cpp` | 单例，整份 config.json 载入内存 `root_`，分节 getter/setter + `save()` |
| `parse_config.hpp/.cpp` | `ConfigParser::parse_arguments` 解析命令行 → `AppConfig` |
| `SharedTypes.hpp` | 共享枚举/常量（`NPU_CORE_NUM`、加速/输入/引擎枚举）与 `AppConfig` |

## 核心类与数据流
- **ConfigManager**（单例）：`load()` 读入 `QJsonObject root_`；各 setter 只改内存，
  `save()` 才整体序列化写回文件。配置节：`video` / `model` / `cascade` / `detect` /
  `alarm` / `geofence`。文件不存在时写入内置默认并 `save()`；JSON 解析失败置空 `root_`，
  getter 回退默认值（0.25 / 0.45 / 80 / 3 / "inside_alarm" …）。
- **ConfigParser**（CLI 用）：解析 `-m/-i/-a/-t/-c/-d/-r/-s/-p/-v/-h` 等选项填充 `AppConfig`，
  缺 `-m`/`-i` 或文件不存在即报错 `exit`。

数据流：
```
GUI/启动 → ConfigManager::load() → 各模块 getter 读取 → setter 改内存 → save() 落盘
CLI 模式 → ConfigParser::parse_arguments(argc,argv) → AppConfig → 检测线程
```

## 使用方法
```cpp
ConfigManager::instance().load("config.json");
double conf = ConfigManager::instance().confThreshold();   // 默认 0.25
ConfigManager::instance().setConfThreshold(0.3);           // 仅改内存
ConfigManager::instance().save();                          // 显式落盘

// 围栏整段由 geofence::FenceManager 经 setGeofenceChannels()+save() 落盘
```
CLI：`./app -m model/yolo11n.rknn -i video.mp4 -t 3 -c true`

## 依赖关系
- Qt：`QFile`/`QJsonDocument`/`QJsonObject`/`QJsonArray`（ConfigManager）。
- `getopt.h`/标准库（ConfigParser）。
- 被 `AlarmManager`、`FenceManager`、检测/模型初始化等广泛引用。

## 注意事项
- **内存态 vs 落盘**：setter 只改 `root_`，不调 `save()` 则重启丢失；但 getter 读同一内存，
  改完立刻可见——"看着生效了其实没存盘"是常见坑。
- **写盘时机与风险**：`save()` 是整文件截断覆写（Indented），非原子；进程在写入中崩溃会
  损坏配置；`save()` 打开文件失败被静默忽略（仅 `load` 失败 qWarning），须保证路径可写。
- **线程安全**：公开接口内部由 `mutex_` 加锁，可跨线程调用；`saveUnsafe()` 仅供已持锁的
  内部路径复用，外部直接调用会与 `save()` 双重加锁死锁（std::mutex 不可重入）。
- **缺省值兜底**：getter 用带默认值的 `toXxx(default)`，缺键不崩、返回兜底；文件损坏时
  `root_` 置空后全走默认，会静默丢弃用户原有配置。
- **通道号约定差异**：ConfigManager 视频通道是 1-based（channel1..4，notes 补齐到 4），
  而报警/围栏侧的 channel 多为 0-based，跨模块传值时注意换算。
- `threads` 默认 3 对应 RK3588 的 3 个 NPU 核心（`NPU_CORE_NUM`）。
