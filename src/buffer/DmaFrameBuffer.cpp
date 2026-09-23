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

/**
 * @brief 带参数的构造函数 - 创建指定尺寸和格式的帧缓冲区
 *
 * @param width:  帧宽度（像素）
 * @param height: 帧高度（像素）
 * @param format: 像素格式（DRM_FORMAT_NV12/RGBA8888 等）
 *
 * @note 仅设置参数，不分配内存。需要调用 alloc() 分配缓冲区。
 */
DmaFrameBuffer::DmaFrameBuffer(int width, int height, int format)
        : m_width(width), m_height(height), m_format(format)
{
    
}

/**
 * @brief 分配 DMA-BUF 缓冲区
 *
 * 调用 alloc_drm_buffer() 通过 DRM 子系统分配物理连续内存。
 * 分配后的缓冲区可同时被 CPU、GPU、RGA、NPU 访问（零拷贝）。
 *
 * @return: DMA-BUF 文件描述符（成功），-1（失败）
 */
int DmaFrameBuffer::alloc()
{
    m_fd = alloc_drm_buffer();
    return m_fd;

}

/**
 * @brief 析构函数 - 自动释放 DMA-BUF 资源
 */
DmaFrameBuffer::~DmaFrameBuffer()
{
    
    release();
}

/**
 * @brief 释放缓冲区资源（munmap + close fd + destroy DRM buffer）
 */
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

    m_stride = create.pitch;   // 实际 stride（DRM 驱动按行对齐后返回，
                               //  通常向上对齐到 64 字节，故可能大于 width*bpp/8，
                               //  后续 RGA/EGL 的 pitch 参数必须用它而非 width）
    m_size = create.size;      // 总分配大小（内核已按页向上取整，含尾部填充）
    
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

/**
 * @brief 释放 DMA 缓冲区（空实现）
 *
 * @note 实际释放逻辑在 free_drm_buffer() 中。
 *       此函数保留作为接口兼容，但不执行任何操作。
 *       DMA 缓冲区通过 DRM DUMB Buffer 分配，释放由 free_drm_buffer() 处理。
 */
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
//
// ⚠【已知缺陷（仅警示）】alloc_drm_buffer() 步骤7 已 close(m_drm_fd) 但
//   未把 m_drm_fd 复位为 -1，且 GEM handle 随 DRM 设备 fd 关闭即失效。
//   因此这里的 DRM_IOCTL_MODE_DESTROY_DUMB 实际是对一个"已关闭（或已被
//   其他线程 open 复用）的整数 fd"发 ioctl：
//   - 通常直接 EBADF 失败，DUMB 内存随 prime fd 关闭由内核回收（侥幸无泄漏）；
//   - 若该 fd 号已被别的文件复用，则属于对无关 fd 的误操作（潜在危害）。
//   正确做法：在 close(m_drm_fd) 后置 m_drm_fd=-1，或保留打开的 DRM fd。
// ⚠ munmap 使用 m_size：若对象经过不完整的移动构造（见头文件警示），
//   m_size 可能为垃圾值，解除映射的长度将不正确。
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
        // 需要重新打开 DRM 设备来释放显存（因为 alloc_drm_buffer 中已关闭 m_drm_fd）
        int drm_fd = open("/dev/dri/card0", O_RDWR);
        if (drm_fd < 0) {
            drm_fd = open("/dev/dri/renderD128", O_RDWR);
        }
        if (drm_fd >= 0) {
            struct drm_mode_destroy_dumb destroy = {};
            destroy.handle = m_handle;
            ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);  // 释放显存
            close(drm_fd);
        }
        m_handle = 0;
    }
    m_size = 0;
    printf("DmaFrameBuffer released.\n");

}

// ============================================================================
// Getter/Setter 方法
// ============================================================================

/** @brief 获取 DMA 缓冲区的文件描述符（用于 RGA/NPU 硬件访问） */
int DmaFrameBuffer::fd() const {return m_fd;}

/** @brief 获取 DMA 缓冲区的虚拟地址指针（用于 CPU 读写） */
uint8_t* DmaFrameBuffer::ptr() const {return m_ptr;}

/** @brief 获取缓冲区大小（字节） */
size_t DmaFrameBuffer::size() const {return m_size;}

/** @brief 获取帧宽度（像素） */
int DmaFrameBuffer::width() const {return m_width;}

/** @brief 设置帧宽度（像素） */
void DmaFrameBuffer::setWidth(int width) {m_width = width;}

/** @brief 获取帧高度（像素） */
int DmaFrameBuffer::height() const {return m_height;}

/** @brief 设置帧高度（像素） */
void DmaFrameBuffer::setHeight(int height) {m_height = height;}

/** @brief 获取像素格式（DRM_FORMAT_NV12/RGBA8888 等） */
int DmaFrameBuffer::format() const {return m_format;}

/** @brief 设置像素格式 */
void DmaFrameBuffer::setFormat(int format) {m_format = format;}

/** @brief 关联 FFmpeg AVFrame 对象（用于硬件解码输出） */
void DmaFrameBuffer::setFrame(AVFrame* frame) {m_frame = frame;}

/** @brief 获取 DRM 缓冲区句柄（用于 DRM ioctl 操作） */
uint32_t DmaFrameBuffer::handle() {return m_handle;}

/** @brief 获取行步长（字节，考虑 GPU 对齐后的实际宽度） */
int DmaFrameBuffer::stride() const {return m_stride;}

