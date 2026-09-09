#include "DmaFrameBuffer.h"

// ============================================================================
// DmaFrameBuffer - DRM DUMB Buffer 分配器
// ============================================================================
//
// 【作用】
//   分配一块 GPU 可访问的 DMA-BUF 内存，用于 RGA 色彩转换的输出缓冲区。
//
// 【为什么用 DRM DUMB Buffer？】
//   - 普通 malloc 分配的内存是 CPU 专用的，GPU/RGA 无法直接访问
//   - DRM DUMB Buffer 是通过 Linux DRM (Direct Rendering Manager) 子系统分配的
//   - 分配的内存可以同时被 CPU（通过 mmap）、GPU、RGA 访问
//   - 这是实现"零拷贝"的关键：多个硬件共享同一块物理内存
//
// 【分配流程】
//   1. open("/dev/dri/card0") → 打开 DRM 设备
//   2. DRM_IOCTL_MODE_CREATE_DUMB → 内核分配连续物理内存
//   3. DRM_IOCTL_PRIME_HANDLE_TO_FD → 将 handle 转换为文件描述符(fd)
//   4. DRM_IOCTL_MODE_MAP_DUMB → 获取内存映射偏移量
//   5. mmap() → 将 GPU 内存映射到 CPU 地址空间
//   6. close(drm_fd) → 关闭设备（fd 仍然有效）
//
// 【为什么分配后要 close(drm_fd)？】
//   - DRM 设备 fd 只在分配时需要
//   - 分配完成后，DMA-BUF fd 是独立的，不再依赖 DRM 设备
//   - 多个 DmaFrameBuffer 共享同一个 drm_fd 会导致竞争
//   - 所以每个 buffer 分配后立即关闭自己的 drm_fd
//
// ============================================================================

DmaFrameBuffer::DmaFrameBuffer(int width, int height, int format)
        : m_width(width), m_height(height), m_format(format)
{
    
}

int DmaFrameBuffer::alloc()
{
    m_fd = alloc_drm_buffer();
    return m_fd;

}

DmaFrameBuffer::~DmaFrameBuffer()
{
    
    release();
}

void DmaFrameBuffer::release ()
{
    free_drm_buffer();
}

// ============================================================================
// 分配 DRM DUMB Buffer
// ============================================================================
// 返回值：DMA-BUF fd（成功）或 -1（失败）
//
// 关键 ioctl 调用：
//   - DRM_IOCTL_MODE_CREATE_DUMB: 分配显存（DUMB = dumb buffer，最基础的显存类型）
//   - DRM_IOCTL_PRIME_HANDLE_TO_FD: 将 DRM handle 转为 PRIME fd（跨设备共享用）
//   - DRM_IOCTL_MODE_MAP_DUMB: 获取 mmap 偏移量（CPU 访问用）
//   - mmap(): 将 GPU 内存映射到 CPU 地址空间（可读可写）
//
int DmaFrameBuffer::alloc_drm_buffer() {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，确保线程安全

    // 步骤1: 打开 DRM 设备
    // /dev/dri/card0: 主 DRM 设备（包含显示控制）
    // /dev/dri/renderD128: 渲染设备（只做渲染，不做显示）
    m_drm_fd = open("/dev/dri/card0", O_RDWR);
    if (m_drm_fd < 0) {
        perror("open /dev/dri/card0");
        return -1;
    }

    // 步骤2: 根据像素格式计算每像素位数 (bpp)
    // NV12: 12 bits/pixel (Y: 8 + UV: 4)
    // RGB888: 24 bits/pixel (R:8 + G:8 + B:8)
    // RGBA8888: 32 bits/pixel (R:8 + G:8 + B:8 + A:8)
    int bpp;
    switch (m_format) {
        case DRM_FORMAT_NV12:
        case DRM_FORMAT_NV21:
            bpp = 12;
            break;
        case DRM_FORMAT_RGB888:
        case DRM_FORMAT_BGR888:
            bpp = 24;
            break;
        default:  // XRGB8888, ARGB8888, RGBA8888 等
            bpp = 32;
            break;
    }

    // 步骤3: 创建 DUMB Buffer（分配显存）
    // DUMB = Display Unified Memory Buffer，最基础的显存类型
    // 内核会分配物理连续的内存（适合 DMA 操作）
    struct drm_mode_create_dumb create = {};
    create.width = m_width;
    create.height = m_height;
    create.bpp = bpp;          // 每像素位数
    create.flags = 0;

    // ioctl: 命令 -> 内核 -> GPU 驱动 -> 分配显存
    // 返回：create.handle（DRM 内部句柄）、create.pitch（实际 stride）、create.size（总大小）
    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        close(m_drm_fd);
        return -1;
    }

    m_stride = create.pitch;   // 实际 stride（可能因对齐而大于 width * bpp/8）
    m_size = create.size;      // 总分配大小
    
    // 步骤4: 将 DRM handle 转换为 PRIME fd
    // DRM handle: 进程内部的句柄，不可跨进程共享
    // PRIME fd: 文件描述符，可以跨进程传递（通过 Unix socket）
    // 这一步是实现"零拷贝"的关键：RGA 通过 fd 访问同一块内存
    int fd = -1;
    struct drm_prime_handle prime_handle = {
        .handle = create.handle,
        .flags = DRM_CLOEXEC,  // O_CLOEXEC: fork 时自动关闭
        .fd = -1,
    };
    m_handle = create.handle;
    
    // ioctl: handle → fd
    if (ioctl(m_drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) != 0) {
        perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
        close(m_drm_fd);
        return -1;
    }
    fd = prime_handle.fd;

    // 步骤5: 获取 mmap 偏移量
    // DRM 内存是 GPU 专用的，不能直接用指针访问
    // 需要先获取偏移量，再通过 mmap 映射到 CPU 地址空间
    struct drm_mode_map_dumb map = {};
    map.handle = create.handle;
    if (ioctl(m_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        close(fd);
        close(m_drm_fd);
        return -1;
    }

    // 步骤6: mmap 映射到 CPU 地址空间
    // 映射后，CPU 可以像访问普通内存一样读写这块 GPU 显存
    // 注意：这是"双重映射"，CPU 和 GPU 可以同时访问同一块物理内存
    // 需要自己做缓存同步（dma_sync_device_to_cpu / dma_sync_cpu_to_device）
    void* ptr = mmap(0, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_drm_fd, map.offset);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        close(m_drm_fd);
        return -1;
    }
    m_ptr = (uint8_t*)ptr;

    // 步骤7: 关闭 DRM 设备（fd 仍然有效）
    // DMA-BUF fd 是独立的，不再依赖 DRM 设备
    close(m_drm_fd);
    return fd;
}

void DmaFrameBuffer::free_dma_buffer() {
    

}

// ============================================================================
// 释放 DRM DUMB Buffer
// ============================================================================
// 释放顺序很重要：
//   1. munmap: 解除 CPU 地址空间映射
//   2. close(fd): 关闭 DMA-BUF fd（引用计数归零时释放内存）
//   3. DRM_IOCTL_MODE_DESTROY_DUMB: 通知内核释放显存
//
// 注意：如果顺序错误（如先 destroy 再 munmap），会导致 use-after-free 崩溃
// ============================================================================
void DmaFrameBuffer::free_drm_buffer() {
    std::lock_guard<std::mutex> lock(dma_mutex);   // 加锁，保护 close/munmap 并发
    if (m_ptr) {
        munmap(m_ptr, m_size);  // 解除 CPU 映射
        m_ptr = nullptr;
    }
    if (m_fd >= 0) {
        close(m_fd);            // 关闭 DMA-BUF fd
        m_fd= -1;
    }
    if (m_handle != 0) {
        struct drm_mode_destroy_dumb destroy = {};
        destroy.handle = m_handle;
        ioctl(m_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);  // 释放显存
        m_handle = 0;
    }
    m_size = 0;
    printf("DmaFrameBuffer released.\n");

}

int DmaFrameBuffer::fd() const {return m_fd;}
uint8_t* DmaFrameBuffer::ptr() const {return m_ptr;}
size_t DmaFrameBuffer::size() const {return m_size;}
int DmaFrameBuffer::width() const {return m_width;}
void DmaFrameBuffer::setWidth(int width) {m_width = width;}
int DmaFrameBuffer::height() const {return m_height;}
void DmaFrameBuffer::setHeight(int height) {m_height = height;}
int DmaFrameBuffer::format() const {return m_format;}
void DmaFrameBuffer::setFormat(int format) {m_format = format;}
void DmaFrameBuffer::setFrame(AVFrame* frame) {m_frame = frame;}
uint32_t DmaFrameBuffer::handle() {return m_handle;}
int DmaFrameBuffer::stride() const {return m_stride;}

