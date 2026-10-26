#ifndef OCR_PIPELINE_DMA_HEAP_H
#define OCR_PIPELINE_DMA_HEAP_H

/** @file dma_heap.h @brief Linux DMA-HEAP allocation helpers.
 *
 * 从 Linux DMA-HEAP 分配共享图像内存，按 16 像素对齐计算行跨度和各平面偏移。
 * heap fd 用完即关闭，分配得到的 DMA-BUF fd 转交缓冲池；map_cpu 只决定是否额外建立 CPU 映射。
 */

#include "buffer_pool.h"

/** Allocate one DMA-BUF. A NULL heap_path tries common system heap names.
 *
 * 指定路径时只尝试该 heap；未指定则按候选顺序尝试，返回 fd 或负数错误。
 */
int ocr_dma_heap_alloc(const char *heap_path, size_t size);

/**
 * @brief 从 DMA-HEAP 创建固定数量、相同图像布局的输出缓冲。
 * @param[out] pool 尚未初始化的缓冲池；成功后接管所有已分配 fd。
 * @param[in] count 缓冲数，每个槽位都分配独立 DMA-BUF，不是对同一 fd 做多次 dup。
 * @param[in] width,height,format 有效图像参数，存储行跨度内部按 16 像素向上对齐。
 * @param[in] heap_path 指定 heap 设备；NULL/空字符串则按候选顺序尝试。
 * @param[in] map_cpu 非零时为每块内存建立 mmap_addr，供 CPU OCR 预处理使用。
 * @return 0 成功；-1 布局/池初始化失败；-2 分配/注册/布局失败；-3 CPU 映射失败。
 * @note 部分失败会回滚已注册资源；成功后通过 pool_acquire/release 复用，
 *       结束所有设备访问并归还引用后，再 pool_destroy 解除映射和关闭 fd。
 */
int ocr_dma_heap_alloc_pool(ocr_buffer_pool_t *pool, int count,
                            uint32_t width, uint32_t height,
                            ocr_pixel_format_t format,
                            const char *heap_path, int map_cpu);

#endif /* OCR_PIPELINE_DMA_HEAP_H */
