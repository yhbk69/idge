# sounds

## 功能概述

报警/拍照相关提示音资源目录。当前仅含**拍照快门音效** `shutter.wav`（人员点名、设备盘点模块拍照时播放），由 `resources.qrc` 注册为 `qrc:/shutter.wav` 编译进主程序。经代码核实：**报警提示目前不播放任何音频文件**（报警为界面 toast/角标，见 `src/ui/form/frmmain.cpp` 的 `alarmToast_`），全工程唯一的声音引用点即本目录的快门音。

## 文件/子目录清单

| 文件 | 说明 |
|------|------|
| `shutter.wav` | 快门"咔嚓"音效（WAV，PCM） |
| `resources.qrc` | RCC 清单（version 1.0），prefix 为空 → 资源别名即 `qrc:/shutter.wav` |

## 使用方法

编译接入（根 `CMakeLists.txt` 第 335~338 行，已验证）：

```cmake
qt5_add_resources(SOUND_RESOURCES
    ${CMAKE_SOURCE_DIR}/sounds/resources.qrc)   # 随 idge 目标一起链接
```

运行时用法（`src/ui/form/photo_selection_widget.cpp:260-263`，已验证的真实接口）：

```cpp
shutter_sound_ = new QSoundEffect(this);
shutter_sound_->setSource(QUrl(QStringLiteral("qrc:/shutter.wav")));
shutter_sound_->setVolume(1.0);
// 拍照时刻（与白色闪光遮罩同步，遮罩 110ms 后自动隐藏）：
shutter_sound_->play();          // photo_selection_widget.cpp:303
```

替换/新增音效：把新 wav 放入本目录、在 `resources.qrc` 追加 `<file>xxx.wav</file>`，重新 `./build-linux.sh -t rk3588 -b Release`。

## 依赖关系

- Qt5 Multimedia 模块（`QSoundEffect`）：CMake 已 `find_package(Qt5 ... Multimedia)` 并链接 `Qt5::Multimedia`；
- 板端音频输出依赖系统 ALSA/PulseAudio 可用（QSoundEffect 走 Qt 多媒体后端）；
- 使用方为 `src/ui/form/photo_selection_widget.cpp/.h`（点名/盘点拍照界面）。

## 注意事项

- 音效文件名即资源别名的一部分：`qrc:/shutter.wav` **不带目录前缀**（resources.qrc 未设 prefix 且文件在 qrc 同级），改名为子目录存放会破坏现有引用。
- `QSoundEffect` 首次 `play()` 前源需加载完成（`status()==Ready`）；在低配板偶发首拍无声时，可在页面初始化后预调用一次 `play(); stop();` 预热。
- 播放音量当前硬编码 1.0；如需可调音量/静音开关，应加到 `AppData`/config.json，而不是改组件代码。
- 若后续为报警增加提示音：与快门音一样放本目录并登记到 `resources.qrc`，避免直接引用文件系统路径（部署目录可能被裁剪）。
