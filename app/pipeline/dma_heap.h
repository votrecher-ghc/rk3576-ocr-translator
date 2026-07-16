#ifndef OCR_PIPELINE_DMA_HEAP_H
#define OCR_PIPELINE_DMA_HEAP_H

/** @file dma_heap.h @brief Linux DMA-HEAP allocation helpers. */

#include "buffer_pool.h"

/** Allocate one DMA-BUF. A NULL heap_path tries common system heap names. */
int ocr_dma_heap_alloc(const char *heap_path, size_t size);

/** Initialize and populate a fixed buffer pool from DMA-HEAP. */
int ocr_dma_heap_alloc_pool(ocr_buffer_pool_t *pool, int count,
                            uint32_t width, uint32_t height,
                            ocr_pixel_format_t format,
                            const char *heap_path, int map_cpu);

#endif /* OCR_PIPELINE_DMA_HEAP_H */
