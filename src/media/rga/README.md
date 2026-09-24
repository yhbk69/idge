# src/media/rga

> 所属域：**media 视频管线域**（目录重组 S4b 迁入）

## 功能概述（在流水线中的位置）
RGA2/RGA3 硬件 2D 加速的封装层，承担两条关键零拷贝路径：① NV12 解码帧 → RGBA 帧（供 CPU 画框 + GLES 渲染）；② NV12/RGBA 帧 → 640×640 RGB 模型输入（含 YOLO letterbox），全部通过 DMA-BUF fd 在设备间直传，CPU 不搬运像素。

## 文件清单
| 文件 | 职责 |
|---|---|
| rga_converter.h | 静态工具类 RgaConverter：色彩转换、缩放、letterbox 预处理、异步转换、纯缩放、空 sync 占位 |

## 核心类与数据流
两条等价 API 风格：
- 轻量版（直接填 `rga_buffer_t.fd`）：`nv12_to_rgba`、`nv12_to_xrgb`、`nv12_to_rgb_resize`、`resize`——每次调用由驱动现场 pin fd。
- 句柄版（`importbuffer_fd → wrapbuffer_handle → im* → releasebuffer_handle`）：`convertNV12ToRGBAbyRGA`、`convertNV12ToRGBbyRGA`、`rgba_to_rgb_resize`——驱动缓存地址表，适合反复使用的缓冲。
- letterbox 流程：scale=min(dst_w/src_w, dst_h/src_h)，(int) 截断求 new_w/new_h，居中偏移 (dst-new)/2；先 `imfill(0x727272)`（0x72=114，YOLO 标准灰）再 `improcess` 缩放+转格式一步完成。
- 数据流：h264_rkmpp 解码出 NV12 fd → RGA 转 RGBA fd →（CPU mmap 画框）→ EGLImage；并行地 RGA 缩 640×640 RGB fd → RKNN 输入。

## 使用方法
```cpp
#include "rga_converter.h"
// 解码帧 → 渲染用 RGBA（同尺寸纯转换，内部自动选 imcvtcolor/improcess）
RgaConverter::nv12_to_rgba(nv12_fd, w, h, stride,
                           rgba_fd, w, h, rgba_stride);
// 模型输入：letterbox 到 640x640
int rc = RgaConverter::nv12_to_rgb_resize(nv12_fd, w, h, stride,
                                          rknn_fd, 640, 640, 640*3, true);
```

## 依赖关系
- 依赖 librga（im2d/im2d.hpp/RgaUtils）与 drm_fourcc；被解码器（reader 模块）、推理预处理、buffer/DmaBufferPool（rga_handle 同源约定）调用。

## 注意事项
- **恒为真返回值（上轮审查确认）**：`convertNV12ToRGBAbyRGA`/`convertNV12ToRGBbyRGA` 在 import 失败、imcheck 失败、imcvtcolor 失败时全部 `return true`，调用方无法感知转换失败（画面表现为旧帧/花屏）；勿依赖返回值判定成败。
- **错误路径句柄泄漏（上轮审查确认）**：`rgba_to_rgb_resize` 的两个转换失败分支 `return -1` 绕过 `release_buffer` 标签，src/dst handle 未释放，RGA 驱动注册表泄漏；其余失败路径又恒返回 0。
- 异步陷阱：`nv12_to_rgba_async` 返回成功仅代表入队，读取 dst 前必须 `imsync()`；本类 `sync()` 是空实现（imsync 被注释），不可依赖。
- `nv12_to_rgb_resize` 非 letterbox 分支误用 `imcvtcolor`（不缩放），需要拉伸请走 letterbox 分支的 improcess。
- 对齐：RGA 要求 NV12 宽高为偶数（4:2:0 色度 2×2 共享）；letterbox (int) 截断可能产生 1px 偏移；wstride 建议 16/32 字节对齐（见 buffer/RGA_ALIGN）。
- `imconfig` 双核调度为进程级全局状态，多线程下互相覆盖；FourCC（RK_FORMAT_YCbCr_420_SP=NV12 等）必须与缓冲实际布局一致，否则按字节错读。
