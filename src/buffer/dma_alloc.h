/**
 * @file dma_alloc.h
 * @brief DMA 缓冲区分配接口
 *
 * 作用：提供 DMA（Direct Memory Access）缓冲区的分配和释放函数。
 *       用于在 Rockchip 平台上分配物理连续内存，支持 RGA 硬件加速
 *       和视频解码器的零拷贝输出。
 */

/*
 * Copyright (C) 2022 Rockchip Electronics Co., Ltd.
 * Authors:
 *  Cerf Yu <cerf.yu@rock-chips.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __RGA_SAMPLES_ALLOCATOR_DMA_ALLOC_H__
#define __RGA_SAMPLES_ALLOCATOR_DMA_HEAP_ALLOC_H__

// ============================================================================
// DMA Heap 路径定义 —— 不同类型的 DMA 内存堆设备节点
// ============================================================================
// 作用：定义 Rockchip 平台上各种 DMA 内存堆的设备路径。
//       不同的堆提供不同的缓存策略和地址空间限制。
// ============================================================================

// 非缓存 DMA 堆：CPU 和设备共享时无需手动同步缓存
#define DMA_HEAP_UNCACHE_PATH           "/dev/dma_heap/system-uncached"
// 普通 DMA 堆：带缓存，CPU 访问更快但需要手动同步
#define DMA_HEAP_PATH                   "/dev/dma_heap/system"
// DMA32 非缓存堆：限制在 32 位地址空间内
#define DMA_HEAP_DMA32_UNCACHE_PATCH    "/dev/dma_heap/system-uncached-dma32"
// DMA32 普通堆：限制在 32 位地址空间内
#define DMA_HEAP_DMA32_PATCH            "/dev/dma_heap/system-dma32"
// CMA 非缓存堆：使用连续内存分配器
#define CMA_HEAP_UNCACHE_PATH           "/dev/dma_heap/cma-uncached"
// RV1106 芯片专用 CMA 堆路径
#define RV1106_CMA_HEAP_PATH	        "/dev/rk_dma_heap/rk-dma-heap-cma"

// ============================================================================
// DMA 缓冲区同步函数
// ============================================================================

/**
 * @brief 设备到 CPU 的缓存同步
 * @param fd DMA 缓冲区文件描述符
 * @return 成功返回 0，失败返回负数
 * 作用：将 DMA 缓冲区的缓存 invalidate 到 CPU，确保 CPU 读取到设备写入的最新数据。
 *       用于设备（如解码器）写入帧数据后，CPU 读取之前调用。
 */
int dma_sync_device_to_cpu(int fd);

/**
 * @brief CPU 到设备的缓存同步
 * @param fd DMA 缓冲区文件描述符
 * @return 成功返回 0，失败返回负数
 * 作用：将 CPU 缓存中的数据 flush 到 DMA 缓冲区，确保设备读取到 CPU 写入的最新数据。
 *       用于 CPU 写入帧数据后，设备（如 RGA）读取之前调用。
 */
int dma_sync_cpu_to_device(int fd);

// ============================================================================
// DMA 缓冲区分配与释放
// ============================================================================

/**
 * @brief 分配 DMA 缓冲区
 * @param path DMA 堆设备路径（如 DMA_HEAP_UNCACHE_PATH）
 * @param size 需要分配的缓冲区大小（字节）
 * @param fd   输出参数，返回 DMA 缓冲区的文件描述符
 * @param va   输出参数，返回映射后的虚拟地址
 * @return 成功返回 0，失败返回负数
 * 作用：通过 DMA Heap 设备分配物理连续内存，并将其映射到用户空间虚拟地址。
 *       分配的内存可被 CPU 和硬件设备（如 RGA、解码器）直接访问。
 */
int dma_buf_alloc(const char *path, size_t size, int *fd, void **va);

/**
 * @brief 释放 DMA 缓冲区
 * @param size 缓冲区大小（字节）
 * @param fd   DMA 缓冲区文件描述符（释放后置为 -1）
 * @param va   映射的虚拟地址（释放后置为 nullptr）
 * 作用：解除虚拟地址映射（munmap）并关闭文件描述符（close），
 *       释放 DMA 缓冲区占用的所有系统资源。
 */
void dma_buf_free(size_t size, int *fd, void *va);

#endif /* #ifndef __RGA_SAMPLES_ALLOCATOR_DMA_ALLOC_H__ */
