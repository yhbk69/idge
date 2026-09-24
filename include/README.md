# include

## 功能概述

头文件搜索路径目录（不参与 GLOB 编译，只提供 `#include`）。当前仅收录 **nlohmann/json**（"JSON for Modern C++"）**v3.11.2** 单头文件库（MIT 协议，头部注释含版本横幅，宏 `NLOHMANN_JSON_VERSION_MAJOR/MINOR/PATCH = 3/11/2`）。用于 C++11+ 的 JSON 解析/序列化，作为 `3rdparty/jsoncpp`（需链接库）的零依赖替代，在人脸识别桥接模块中使用。

## 文件/子目录清单

| 文件/目录 | 说明 |
|-----------|------|
| `nlohmann/json.hpp` | 全部功能合并为单头文件（约 90 万+字符），直接 include 即可，无需编译/链接额外库 |

## 使用方法

根 `CMakeLists.txt` 已将 `${CMAKE_SOURCE_DIR}/include` 加入头文件路径，任一源文件中：

```cpp
#include "nlohmann/json.hpp"
using json = nlohmann::json;

json j = json::parse(R"({"conf":0.25,"cls":"person"})");   // 解析
float conf = j["conf"];                                     // 读取
std::string s = j.dump();                                   // 序列化
```

项目内真实用例：`src/ai/recognition/face_recognizer.cpp:15` `#include "nlohmann/json.hpp"`。

## 依赖关系

- 仅依赖 C++11 标准库（`<string>/<vector>/<map>` 等），无外部库、无链接项；
- 被 `src/ai/recognition/` 使用；其余 JSON 场景当前走 `3rdparty/jsoncpp`（config/报警等）与 Qt `QJsonDocument`（UI 配置）；
- 升级替换：从上游 release 覆盖 `nlohmann/json.hpp` 单文件即可（保持目录名 `nlohmann/`，include 写法不变）。

## 注意事项

- 单头文件编译开销大（首文件 include 约秒级），只在需要的 .cpp 中引入，勿放入公共头文件。
- 版本 3.11.2 中已知问题：与部分旧 GCC 的 `-Wreserved-identifier` 及 C++20 `requires` 组合偶有告警；如需升级请整体替换文件并全量回归 `face_recognizer`。
- 与 jsoncpp 并存时注意命名冲突：本项目统一 `using json = nlohmann::json;` 只放在使用它的 .cpp 内部。
- 解析不可信输入时使用 `json::parse(s, nullptr, false)` 并检查 `is_discarded()`，避免默认异常模式在嵌入式端抛出。
