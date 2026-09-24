# src/base/utils

> 所属域：**base 基础设施域**（目录重组 S4a 迁入，依赖方向 base ← media/ai/biz/ui）

## 功能概述

底层工具库，为 IDGE（RK3588 施工行为监测与分析系统）的识别流水线、检测可视化和业务服务提供**与 OpenCV 解耦的 C 风格图像 I/O、硬件加速转换、软件绘制、路径/配置解析、日志与任务目录管理**等基础能力。多数函数源自 rknn_model_zoo 示例并做了本地化改造（RGA 硬件加速回退、ImageMagick 解码兜底、任务目录去重命名）。这些头/源文件多为无状态工具（少数全局开关如 `g_log_level`），可被任意模块直接包含调用。

## 文件清单

| 文件 | 职责 |
| --- | --- |
| `file_utils.h/.cpp` | 文件读取原语：`load_model`（RKNN 模型整体读入内存）、`read_data_from_file`、`write_data_to_file`、`read_lines_from_file`/`free_lines`（标签列表逐行读取，需配对释放） |
| `image_utils.h/.cpp` | 图像读写/格式转换/缩放裁剪：`read_image`、`write_image`、`convert_image`、`convert_image_with_letterbox`、`get_image_size`、`getLetter`；JPEG 走 turbojpeg，缩放优先 RGA 硬件、失败回退 CPU |
| `image_drawing.h/.cpp` | 无 OpenCV 依赖的软件绘制：点/线/圆/矩形/旋转框(OBB)/文字，覆盖 RGB888/RGBA8888/YUV420SP 帧格式，供 RKNN 输出 buffer 直接可视化 |
| `font.h` | 纯数据：95 个 ASCII 字符（0x20~0x7E）等宽点阵字模 `mono_font_data[95][40*20]`，供 `image_drawing` 的 `draw_text` 双线性缩放后按灰度做 alpha 混合 |
| `draw_utils.h/.cpp` | OpenCV 版画框封装：绿色实线框（唯一/正常）、黄色虚线框（重复）、人脸区域裁剪 `cropFaceRegion`，供点名与盘点结果标注 |
| `qt_image_utils.h` | Qt 安全解码：`loadPixmapSafe` 用 OpenCV 解码规避板端 Qt JPEG/libjpeg ABI 冲突，失败再回退 ImageMagick `convert` |
| `path_utils.h/.cpp` | 路径/配置解析：环境变量读取、可执行文件目录、默认搜索根、多候选路径回退解析（`MYDEMO_ASSET_ROOT` 优先） |
| `xml_utils.h/.cpp` | 指令 XML 解析：UTF-8↔GB2312 转码（iconv）、正则提取设备 `SN` 与 `CmdType` 字段 |
| `task_manager.h/.cpp` | 任务目录/文件管理：建带 `_N` 后缀去重的任务文件夹、"时间戳+随机数"唯一文件名、图片落盘、列目录图片、删任务夹 |
| `logging.h` | 分级日志宏 `NN_LOG_ERROR/WARNING/INFO/DEBUG`，由全局 `g_log_level`（默认 4=全开）控制输出 |

## 核心类与流程

- **图像解码主链路（`image_utils::read_image`）**：按扩展名分派 → JPEG 用 turbojpeg 解为 RGB888 →（可选）RGA 硬件做缩放/裁剪（源与目标宽高需 **16 像素对齐**，不满足或调用失败则回退 `convert_image` 的 CPU 实现）→ PNG/BMP 走 stb。`convert_image_with_letterbox` 先 `getLetter` 算等比缩放 scale 与居中 x_pad/y_pad，再保比例缩放并补边（color 决定填充灰度），得到模型输入尺寸（如 640×640）。

- **Qt 侧兜底解码（`qt_image_utils::loadPixmapSafe`）**：`cv::imread` → 空则 `QProcess` 调 `convert` → 再转 `QPixmap`。用于 UI 展示照片，绕开板端 Qt 图像插件崩溃。

- **检测框绘制**：OpenCV 路径用 `DrawUtils`（实线/虚线区分重复与否，`cropFaceRegion` 与图像矩形求交裁剪防越界）；裸 buffer 路径用 `image_drawing`（`convert_color` BT.601 换算、距离场画线画圆、`rbbox_to_corners` 还原 OBB 四角、`draw_text` 读 `font.h` 字模经 `resize_bilinear_c1` 缩放到目标字号做 alpha 混合）。

- **任务目录组织（`task_manager`）**：`createTaskFolder` 建夹并对同名任务追加 `_1/_2…`；`generateUniqueFilename(prefix, ext)` 用时间戳+随机数避免覆盖；`saveImageToFolder`、`getImagePathsInFolder`、`deleteTaskFolder` 配套。被点名/盘点服务复用。

- **配置定位（`path_utils`）**：`resolve_config_path`/`resolve_existing_path` 依 `MYDEMO_ASSET_ROOT` 环境变量、可执行文件目录、若干相对候选依次回退，命中即返回绝对路径，提升部署鲁棒性。

## 使用方法

```cpp
#include "image_utils.h"
#include "draw_utils.h"
#include "task_manager.h"
#include "qt_image_utils.h"
#include "logging.h"

// 1. 读图 + letterbox 预处理为模型输入
image_buffer_t img = {0};
read_image("/path/to/photo.jpg", &img);            // 解为 RGB888
image_buffer_t model_in = {0};
letterbox_t lb;
convert_image_with_letterbox(&img, &model_in, &lb, 114);  // 等比缩放+灰边到输入尺寸

// 2. 裸 buffer 软件绘制（不依赖 OpenCV；color 为 ARGB8888，thickness=-1 表填充）
image_buffer_t draw_img = model_in;                // copy buffer 后绘制
draw_rectangle(&draw_img, x, y, w, h, COLOR_GREEN, 2);
draw_text(&draw_img, "person 0.91", x, y - 12, COLOR_YELLOW, 16);

// 3. OpenCV Mat 上区分唯一/重复框
cv::Mat face = DrawUtils::cropFaceRegion(bgr, cv::Rect(x,y,w,h));
DrawUtils::drawSolidGreenBox(bgr, box);            // 唯一人脸
DrawUtils::drawDashedYellowBox(bgr, dupBox);       // 重复人脸

// 4. 任务目录与唯一文件名
std::string folder = TaskManager::createTaskFolder("./roll_call_data", "第一批");
std::string name   = TaskManager::generateUniqueFilename("processed", ".jpg");

// 5. UI 安全解码 & 日志
QPixmap pm = loadPixmapSafe(QString::fromStdString(photoPath));
NN_LOG_INFO("loaded %s", photoPath.c_str());       // 受 g_log_level 控制
```

## 依赖关系

- **第三方库**：OpenCV（`draw_utils`、`qt_image_utils`、部分读图）、turbojpeg + stb（`image_utils` 解码后端）、Qt5（`qt_image_utils` 的 QImage/QProcess）、iconv（`xml_utils` GB2312 转码）、RKNN/rknn_model_zoo 头（`file_utils`/`image_utils` 的 `image_buffer_t`/`_rect_t`/格式枚举、`rknn_inference` 数据类型）。
- **Rockchip 专有**：`im2d.h`/`drmrga.h` 提供 RGA 2D 硬件加速（缩放/格式转换），运行时不可用则自动 CPU 回退。
- **被上层使用**：`src/biz/service`（点名/盘点服务用 task_manager、draw_utils、qt_image_utils）、`src/ai/recognition`、`src/ui/form` 界面层与识别流水线各阶段。
- **内部自洽**：`image_drawing` 依赖 `font.h`（同目录）；`draw_utils` 独立于 `image_drawing`（一个走 OpenCV、一个走裸 buffer，二者不互含）。

## 注意事项

1. **RGA 16 像素对齐**：走硬件缩放时源/目标宽高需 16 对齐，否则内部回退 CPU——性能路径敏感场景应对齐输入尺寸；调试打印 `write_image path: …` 可判断实际通道数。
2. **`get_image_size` / `read_image_jpeg` 潜在边界**（代码注释已标注、逻辑保持原样）：`get_image_size` 遇未知格式 switch 后无 return 属未定义行为；`read_image_jpeg` 中 `fopen` 失败仅打印未提前返回，随后对 NULL 指针 fseek 可能段错误——调用方须保证路径有效、格式受支持。
3. **字模与绘制耦合**：`font.h` 是 40×20 超采样灰度位阵，仅覆盖 ASCII 0x20~0x7E，中文/特殊符号无法绘制；改字号靠 `draw_text` 内双线性缩放，请勿手工编辑 `font.h` 数据。
4. **解码兜底**：板端 OpenCV JPEG 后端偶发崩溃，故 UI 侧统一用 `loadPixmapSafe`（再兜底 ImageMagick），保存图用 `QImage::save`/自封装而非 `cv::imwrite`——不要绕开这些封装直接调 OpenCV 编解码。
5. **资源配对释放**：`load_model`、`read_data_from_file`、`read_lines_from_file` 返回堆内存，调用方必须 `free`/`free_lines` 对应释放，否则RKNN模型加载等高频路径会内存泄漏。
6. **日志级别**：`g_log_level` 为 `static`（每编译单元独立），默认 4（DEBUG 全开）；生产调优需在包含处统一约定，跨文件不会自动同步。
