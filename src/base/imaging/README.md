# src/base/imaging

> 所属域：**base 基础设施域**（依赖方向 base ← media/ai/biz/ui）。
> 前身是 `src/base/utils`——那个目录里同时住着 C 风格图像 I/O、软件绘制、Qt 解码兜底、
> 路径解析、日志宏和"点名/盘点任务目录管理"，八类东西一个抽屉。
> 2026-10-29 拆分：图像相关的留下，`task_manager` 归位 `src/biz/service/`（唯二调用者都在那），
> `perf_probe.h` 上移到 `src/base/`（与 `runtime_paths.h` 同级），
> 零调用的 `logging.h`/`path_utils.*`/`xml_utils.*` 与 `file_utils.cpp` 里未声明就没人调的 `load_model` 一并删除。

## 功能概述

与 OpenCV 解耦的 C 风格图像 I/O、RGA 硬件加速转换、软件绘制、以及 UI 侧的 Qt 安全解码。
多数函数源自 rknn_model_zoo 示例并做了本地化改造（RGA 失败回退 CPU、ImageMagick 解码兜底）。
全部为无状态工具，可被任意模块直接包含。

## 文件清单

| 文件 | 职责 |
| --- | --- |
| `file_utils.h/.cpp` | 文件内容读写原语：`read_data_from_file`、`write_data_to_file`、`read_lines_from_file`/`free_lines`（标签列表逐行读取，需配对释放） |
| `image_utils.h/.cpp` | 图像读写/格式转换/缩放裁剪：`read_image`、`write_image`、`convert_image`、`convert_image_with_letterbox`、`get_image_size`、`getLetter`；JPEG 走 turbojpeg，缩放优先 RGA 硬件、失败回退 CPU |
| `image_drawing.h/.cpp` | 裸 buffer 软件绘制：点/线/圆/矩形/旋转框(OBB)/文字，覆盖 RGB888/RGBA8888/YUV420SP，供 RKNN 输出 buffer 直接可视化 |
| `font.h` | 纯数据：95 个 ASCII 字符（0x20~0x7E）等宽点阵字模 `mono_font_data[95][40*20]`，供 `draw_text` 双线性缩放后按灰度做 alpha 混合 |
| `draw_utils.h/.cpp` | OpenCV 版画框封装：绿色实线框（唯一/正常）、黄色虚线框（重复）、人脸区域裁剪 `cropFaceRegion`，供点名与盘点结果标注 |
| `qt_image_utils.h` | header-only 的 Qt 安全解码：`loadPixmapSafe` 用 OpenCV 解码规避板端 Qt JPEG/libjpeg ABI 冲突，失败再回退 ImageMagick `convert`；另有 `thumbnailSidecarPath`/`loadThumbnailScaled` |

同批搬走的两件（不在本目录）：`src/biz/service/task_manager.h/.cpp`（任务目录/唯一文件名/图片落盘，
业务侧产物组织规则）、`src/base/perf_probe.h`（`IDGE_PERF`/`IDGE_NODRAW` 环境变量采样开关）。

## 核心类与流程

- **图像解码主链路（`image_utils::read_image`）**：按扩展名分派 → JPEG 用 turbojpeg 解为 RGB888 →
  （可选）RGA 硬件做缩放/裁剪（源与目标宽高需 **16 像素对齐**，不满足或调用失败则回退 `convert_image` 的 CPU 实现）
  → PNG/BMP 走 stb。`convert_image_with_letterbox` 先 `getLetter` 算等比缩放 scale 与居中 x_pad/y_pad，
  再保比例缩放并补边（color 决定填充灰度），得到模型输入尺寸（如 640×640）。
- **Qt 侧兜底解码（`qt_image_utils::loadPixmapSafe`）**：`cv::imread` → 空则 `QProcess` 调 `convert` → 再转 `QPixmap`。
  用于 UI 展示照片，绕开板端 Qt 图像插件崩溃。
- **检测框绘制有两条不相交的路**：OpenCV Mat 走 `DrawUtils`（实线/虚线区分重复与否，`cropFaceRegion` 与图像矩形求交裁剪防越界）；
  裸 buffer 走 `image_drawing`（`convert_color` BT.601 换算、距离场画线画圆、`rbbox_to_corners` 还原 OBB 四角、
  `draw_text` 读 `font.h` 字模经 `resize_bilinear_c1` 缩放后做 alpha 混合）。

## 使用方法

```cpp
#include "image_utils.h"
#include "draw_utils.h"
#include "qt_image_utils.h"

// 1. 读图 + letterbox 预处理为模型输入
image_buffer_t img = {0};
read_image("/path/to/photo.jpg", &img);                     // 解为 RGB888
image_buffer_t model_in = {0};
letterbox_t lb;
convert_image_with_letterbox(&img, &model_in, &lb, 114);    // 等比缩放+灰边到输入尺寸

// 2. 裸 buffer 软件绘制（color 为 ARGB8888，thickness=-1 表填充）
image_buffer_t draw_img = model_in;                        // copy buffer 后绘制
draw_rectangle(&draw_img, x, y, w, h, COLOR_GREEN, 2);
draw_text(&draw_img, "person 0.91", x, y - 12, COLOR_YELLOW, 16);

// 3. OpenCV Mat 上区分唯一/重复框
cv::Mat face = DrawUtils::cropFaceRegion(bgr, cv::Rect(x, y, w, h));
DrawUtils::drawSolidGreenBox(bgr, box);                     // 唯一人脸
DrawUtils::drawDashedYellowBox(bgr, dupBox);                // 重复人脸

// 4. UI 安全解码
QPixmap pm = loadPixmapSafe(QString::fromStdString(photoPath));
```

## 依赖关系

- **第三方库**：OpenCV（`draw_utils`、`qt_image_utils`、部分读图）、turbojpeg + stb（`image_utils` 解码后端）、
  Qt5（`qt_image_utils` 的 QImage/QProcess）、RKNN/rknn_model_zoo 头（`file_utils`/`image_utils` 的
  `image_buffer_t`/`_rect_t`/格式枚举）。
- **Rockchip 专有**：`im2d.h`/`drmrga.h` 提供 RGA 2D 硬件加速，运行时不可用则自动 CPU 回退。
- **被上层使用**：`src/ai/yolo11`（`yolo11_model.hpp` 用 `read_data_from_file` 读 .rknn、用 `letterbox_t`）、
  `src/media/reader`（`image_utils`/`image_drawing`）、`src/biz/service`（`draw_utils`/`qt_image_utils`）、
  `src/ui/form`+`src/ui/widgets`（`qt_image_utils`）。
- **内部自洽**：`image_utils.cpp` 复用同目录 `file_utils.h`，`image_drawing.cpp` 复用同目录 `font.h`；
  `draw_utils` 与 `image_drawing` 互不包含（一个 OpenCV、一个裸 buffer）。
- ⚠ **已知域倒置（未修）**：`image_utils.h` 与 `image_drawing.h` include `common.hpp`，
  而 `common.hpp` 位于 `src/ai/yolo11/`，于是 base 头把 OpenCV + rknn_api.h + DmaBufferPool.h 一起带进来了。
  按 `base ← ai` 的依赖方向这是反向的；要修得把 `image_buffer_t`/`_rect_t` 这类纯类型定义从 `common.hpp` 剥到 base，
  属另一件事，勿在本目录里继续加码。

## 注意事项

1. **RGA 16 像素对齐**：走硬件缩放时源/目标宽高需 16 对齐，否则内部回退 CPU——性能路径敏感场景应对齐输入尺寸。
2. **`get_image_size` / `read_image_jpeg` 潜在边界**（代码注释已标注、逻辑保持原样）：`get_image_size` 遇未知格式
   switch 后无 return 属未定义行为；`read_image_jpeg` 中 `fopen` 失败仅打印未提前返回，随后对 NULL 指针 fseek 可能段错误
   ——调用方须保证路径有效、格式受支持。
3. **字模与绘制耦合**：`font.h` 是 40×20 超采样灰度位阵，仅覆盖 ASCII 0x20~0x7E，中文/特殊符号无法绘制；
   改字号靠 `draw_text` 内双线性缩放，请勿手工编辑 `font.h` 数据。
4. **解码兜底**：板端 OpenCV JPEG 后端偶发崩溃，故 UI 侧统一用 `loadPixmapSafe`（再兜底 ImageMagick），
   保存图用 `QImage::save`/自封装而非 `cv::imwrite`——不要绕开这些封装直接调 OpenCV 编解码。
5. **资源配对释放**：`read_data_from_file`、`read_lines_from_file` 返回堆内存，
   调用方必须 `free`/`free_lines` 对应释放，否则模型加载等高频路径会内存泄漏。
