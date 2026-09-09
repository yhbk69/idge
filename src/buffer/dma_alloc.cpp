/*
 * Copyright (C) 2022 Rockchip Electronics Co., Ltd.
 *
 * ============================================================================
 * DMA-BUF Heap 分配器（Rockchip 平台）
 * ============================================================================
 *
 * 【作用】
 *   通过 Linux DMA-BUF Heap 机制分配物理连续的内存块。
 *   分配的内存可以被多个硬件设备（CPU、GPU、RGA、NPU）共享访问。
 *
 * 【与 DmaFrameBuffer 的区别】
 *   - DmaFrameBuffer: 使用 DRM DUMB Buffer（通过 /dev/dri/card0）
 *   - dma_buf_alloc(): 使用 DMA-BUF Heap（通过 /dev/dma_heap/...）
 *   - 两者都能分配 DMA-BUF，但底层机制不同
 *   - DMA-BUF Heap 是较新的 Linux 内核接口（5.6+）
 *
 * 【DMA-BUF 同步机制】
 *   CPU 和 GPU 是异步执行的，各自有缓存。如果 CPU 写了数据，GPU 可能还在读旧数据。
 *   需要显式同步：
 *     - dma_sync_cpu_to_device(): CPU 写完 → 通知 GPU "数据已更新，可以读了"
 *     - dma_sync_device_to_cpu(): GPU 写完 → 通知 CPU "数据已更新，可以读了"
 *   这类似于线程间的 memory barrier。
 *
 * 【内存布局】
 *   分配的内存是物理连续的，适合 DMA 传输：
 *   - GPU 可以直接读写，无需 CPU 拷贝
 *   - RGA 可以直接做色彩转换
 *   - NPU 可以直接做推理
 *
 * ============================================================================ */

#include <getopt.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <stdbool.h>
#include <assert.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/eventfd.h>

#include <sched.h>
#include <pthread.h>

#include <stdint.h>
#include <math.h>
#include <memory.h>
#include <sys/time.h>

#include "dma_alloc.h"
#include "rga/RgaUtils.h"

typedef unsigned long long __u64;
typedef  unsigned int __u32;

// ============================================================================
// DMA-BUF Heap 分配数据结构
// ============================================================================
// 用于与内核通信，请求分配 DMA-BUF 内存
//   - len: 要分配的字节数
//   - fd: 分配成功后返回的文件描述符
//   - fd_flags: fd 的打开标志（通常 O_CLOEXEC | O_RDWR）
//   - heap_flags: 堆标志（通常为 0）
// ============================================================================
struct dma_heap_allocation_data {
	__u64 len;
	__u32 fd;
	__u32 fd_flags;
	__u64 heap_flags;
};

#define DMA_HEAP_IOC_MAGIC		'H'
#define DMA_HEAP_IOCTL_ALLOC	_IOWR(DMA_HEAP_IOC_MAGIC, 0x0,\
				      struct dma_heap_allocation_data)

// ============================================================================
// DMA-BUF 同步标志
// ============================================================================
// 用于在 CPU 和 GPU 之间同步内存访问
//
// DMA_BUF_SYNC_START: 开始同步（CPU 准备读写）
// DMA_BUF_SYNC_END:   结束同步（CPU 完成读写，GPU 可以继续）
//
// 类比：
//   CPU 写数据时：
//     1. DMA_BUF_SYNC_START → 告诉 GPU "我要开始写了，别来读"
//     2. CPU 写入数据
//     3. DMA_BUF_SYNC_END → 告诉 GPU "我写完了，你可以读了"
//   GPU 写数据时：反过来
// ============================================================================
#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)

struct dma_buf_sync {
	__u64 flags;
};

#define DMA_BUF_BASE		'b'
#define DMA_BUF_IOCTL_SYNC	_IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)

#define CMA_HEAP_SIZE	1024 * 1024

// ============================================================================
// DMA-BUF 同步函数
// ============================================================================
// 这两个函数用于解决 CPU 和 GPU 的缓存一致性问题
//
// 为什么需要同步？：
//   - CPU 有 L1/L2 缓存，GPU 也有缓存
//   - 如果 CPU 写了数据但没刷新缓存，GPU 读到的还是旧数据
//   - 这两个函数会刷新 CPU 缓存，确保 GPU 看到最新数据
//
// 注意：在纯 GPU 操作（如 RGA→RGA）时不需要同步，
//       只有 CPU 参与读写时才需要
// ============================================================================
int dma_sync_device_to_cpu(int fd) {
    struct dma_buf_sync sync = {0};

    // START + RW: 通知内核 "CPU 准备读写，请刷新 GPU 缓存"
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int dma_sync_cpu_to_device(int fd) {
    struct dma_buf_sync sync = {0};

    // END + RW: 通知内核 "CPU 写完了，请让 GPU 看到最新数据"
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

// ============================================================================
// 分配 DMA-BUF 内存
// ============================================================================
// 参数：
//   - path: DMA-BUF Heap 设备路径
//           常见路径：
//             /dev/dma_heap/system       → 系统堆（普通内存，可能不连续）
//             /dev/dma_heap/system-uncached → 无缓存堆（适合 DMA）
//             /dev/dma_heap/rockchip-rgra → RGA 专用堆（物理连续）
//   - size: 要分配的字节数
//   - fd:   输出参数，分配成功后的文件描述符
//   - va:   输出参数，映射后的 CPU 虚拟地址
//
// 返回值：0 成功，负数失败
//
// 分配流程：
//   1. open(path) → 打开 DMA-BUF Heap 设备
//   2. ioctl(DMA_HEAP_IOCTL_ALLOC) → 内核分配物理连续内存
//   3. mmap() → 将 GPU 内存映射到 CPU 地址空间
//   4. close(heap_fd) → 关闭 Heap 设备（DMA-BUF fd 仍然有效）
// ============================================================================
int dma_buf_alloc(const char *path, size_t size, int *fd, void **va) {
    int ret;
    int prot;
    void *mmap_va;
    int dma_heap_fd = -1;
    struct dma_heap_allocation_data buf_data;

    // 步骤1: 打开 DMA-BUF Heap 设备
    dma_heap_fd = open(path, O_RDWR);
    if (dma_heap_fd < 0) {
        printf("open %s fail!\n", path);
        return dma_heap_fd;
    }

    // 步骤2: 请求内核分配 DMA-BUF 内存
    memset(&buf_data, 0x0, sizeof(struct dma_heap_allocation_data));

    buf_data.len = size;
    buf_data.fd_flags = O_CLOEXEC | O_RDWR;
    // ioctl: 向内核发送分配请求
    // 内核会在指定的 Heap 上分配物理连续内存
    // 分配成功后，buf_data.fd 是指向这块内存的文件描述符
    ret = ioctl(dma_heap_fd, DMA_HEAP_IOCTL_ALLOC, &buf_data);
    if (ret < 0) {
        printf("RK_DMA_HEAP_ALLOC_BUFFER failed\n");
        return ret;
    }

    // 步骤3: mmap 映射到 CPU 地址空间
    // 检查 fd 是否有读写权限
    if (fcntl(buf_data.fd, F_GETFL) & O_RDWR)
        prot = PROT_READ | PROT_WRITE;
    else
        prot = PROT_READ;

    // mmap: 将 GPU 可访问的内存映射到 CPU 地址空间
    // 映射后，CPU 可以像访问普通内存一样读写这块 GPU 显存
    mmap_va = (void *)mmap(NULL, buf_data.len, prot, MAP_SHARED, buf_data.fd, 0);
    if (mmap_va == MAP_FAILED) {
        printf("mmap failed: %s\n", strerror(errno));
        return -errno;
    }

    *va = mmap_va;   // 输出：CPU 虚拟地址
    *fd = buf_data.fd;  // 输出：DMA-BUF 文件描述符

    // 步骤4: 关闭 Heap 设备（DMA-BUF fd 仍然有效）
    close(dma_heap_fd);

    return 0;
}

// ============================================================================
// 释放 DMA-BUF 内存
// ============================================================================
// 释放流程：
//   1. munmap: 解除 CPU 地址空间映射
//   2. close(fd): 关闭 DMA-BUF fd（引用计数归零时内核释放物理内存）
// ============================================================================
void dma_buf_free(size_t size, int *fd, void *va) {
    int len;

    len =  size;
    munmap(va, len);  // 解除 CPU 映射

    close(*fd);       // 关闭 fd，内核释放物理内存
    *fd = -1;
}



