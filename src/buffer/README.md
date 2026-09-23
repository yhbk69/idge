# src/buffer

## 功能概述（在流水线中的位置）
零拷贝视频流水线的"内存底座"。FFmpeg(h264_rkmpp) 硬解码输出 NV12 DMA-BUF、RGA 硬件转换/缩放产生的 RGBA 与 640×640 输入帧，全部落在本目录分配或池化管理的 DMA-BUF 物理连续内存上。下游（RGA、NPU、GPU/EGLImage）都以 fd 共享同一物理页，CPU 只在画框时 mmap 触点。

## 文件清单
| 文件 | 职责 |
|---|---|
| dma_alloc.h / dma_alloc.cpp | Linux DMA-BUF Heap 分配器（open /dev/dma_heap/* + ioctl + mmap）与 CPU↔设备缓存同步 |
| DmaBufferPool.h | 线程安全 DMA 缓冲池（acquire/tryAcquire/release + shared_ptr 自动归还 + RAII Guard） |
| DmaFrameBuffer.h / .cpp | 单个 DMA 帧缓冲的 RAII 封装，走 DRM DUMB Buffer（/dev/dri/card0 + PRIME fd）路径 |
| FrameBuffer.h | 帧元信息结构体（与 DmaFrameBuffer.h 同名不同类的"数据载体版"） |

## 核心类与数据流
- `dma_buf_alloc(path,size,&fd,&va)`：ioctl 分配 → mmap 映射 → 返回 (fd, va)。fd 是跨设备通行证：RGA `importbuffer_fd`、EGL `eglCreateImageKHR`、NPU 输入都只传 fd。
- `DmaBufferPool`：构造期一次性分配 capacity 个 `DmaBuffer`（含预导入的 rga_handle），放入可用队列；`acquire()` 阻塞取队首、`release()` 归还并 `notify_one`，均 O(1)。`tryAcquireSharedPtr()` 用自定义删除器把"delete"重定向为"归还池"，配合 weak_ptr 防环。
- 帧流转：解码器持有 NV12 fd →（dup）→ 池中的 RGBA 缓冲被 RGA 写入 → CPU mmap 画框 → EGLImage 送 GLES 渲染 → 渲染签收后 release 回池。

## 使用方法
```cpp
#include "DmaBufferPool.h"
auto pool = std::make_shared<DmaBufferPool>(
    8, 1920, 1080, RK_FORMAT_RGBA_8888, /*align=*/32);
DmaBuffer* buf = pool->acquire();          // 阻塞借出
rga_buffer_t d = wrapbuffer_handle(buf->rga_handle,
                                   buf->width, buf->height, buf->format);
// ... RGA/EGL 使用 buf->fd ...
pool->release(buf);                        // 必须归还；或
auto sp = pool->tryAcquireSharedPtr();     // 智能指针版本：出作用域自动归还
```
DMA Heap 直接分配：
```cpp
int fd; void* va;
dma_buf_alloc(DMA_HEAP_UNCACHE_PATH, size, &fd, &va);
dma_sync_device_to_cpu(fd);  /* CPU 读前 */
dma_sync_cpu_to_device(fd);  /* CPU 写后、设备读前 */
dma_buf_free(size, &fd, va);
```

## 依赖关系
- 被引用：src/rga（格式/句柄约定）、src/queue/frame_queue.h（持池引用）、解码与渲染链路（DmaFrameBuffer/FrameBuffer 作为信号载荷）。
- 外部依赖：librga（im2d/RgaUtils）、Linux dma_heap 与 DRM 头、FFmpeg（仅头文件，关联 AVFrame）。

## 注意事项
- **fd 所有权**：`dma_buf_alloc` 成功后 (fd,va) 归调用方，只能 free 一次；跨线程共享一律 `dup(fd)`，各持有者各自 close（dma-buf 内核引用计数保证最后关闭者回收）。
- **浅拷贝双释放（已修复）**：`DmaFrameBuffer` 拷贝构造/赋值已被 `= delete` 禁用（历史上为 default 浅拷贝，副本析构会对同一 fd 二次 close、同一区间二次 munmap）。需要共享时用 `std::shared_ptr<DmaFrameBuffer>` 或显式 dup(fd)。
- **移动构造不完整（警示）**：只转移 m_fd/m_ptr，m_size/m_handle/m_drm_fd 未转移未置空，被移动对象析构可能重复销毁；free_drm_buffer 还在已 close 的 m_drm_fd 上发 DESTROY_DUMB ioctl。
- **裸指针生命周期（上轮审查确认）**：`DmaBufferPool::acquire()` 返回的 `DmaBuffer*` 仅在使用方 release 前、池析构前有效；池销毁后为悬垂指针，借出未还的缓冲还会泄漏。
- 池析构只回收"已归还"的缓冲；`free_dma_buffer` 中 reset() 先清零 fd/va/size 导致 munmap/close 实际失败（注释已警示）。
- 对齐：`RGA_ALIGN` 要求 align 为 2 的幂；stride 用 32 字节，NV12 隐含宽高偶数要求。
- 缓存同步：uncached 堆无需 sync；cached 堆必须 device_to_cpu/cpu_to_device 成对使用；纯设备间（RGA→RGA）不需要。
