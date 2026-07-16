/** @file dma_heap.c @brief Linux DMA-HEAP allocation implementation. */
#include "dma_heap.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define OCR_DMA_STRIDE_ALIGN 16U

static size_t image_layout(uint32_t width, uint32_t height,
                           ocr_pixel_format_t format,
                           uint32_t *plane_count,
                           uint32_t strides[4], uint32_t offsets[4])
{
    if (width == 0 || height == 0 || !plane_count || !strides || !offsets ||
        width > UINT32_MAX - (OCR_DMA_STRIDE_ALIGN - 1U)) return 0;
    memset(strides, 0, sizeof(uint32_t) * 4U);
    memset(offsets, 0, sizeof(uint32_t) * 4U);
    uint32_t pixel_stride = (width + OCR_DMA_STRIDE_ALIGN - 1U) &
                            ~(OCR_DMA_STRIDE_ALIGN - 1U);
    uint64_t bytes;
    uint64_t stride_bytes;
    *plane_count = 1;
    switch (format) {
    case OCR_FMT_NV12:
        if ((width & 1U) || (height & 1U)) return 0;
        *plane_count = 2;
        strides[0] = strides[1] = pixel_stride;
        bytes = (uint64_t)pixel_stride * height;
        if (bytes > UINT32_MAX) return 0;
        offsets[1] = (uint32_t)bytes;
        bytes += (uint64_t)pixel_stride * (height / 2U);
        break;
    case OCR_FMT_NV16:
        if (width & 1U) return 0;
        *plane_count = 2;
        strides[0] = strides[1] = pixel_stride;
        bytes = (uint64_t)pixel_stride * height;
        if (bytes > UINT32_MAX) return 0;
        offsets[1] = (uint32_t)bytes;
        bytes += (uint64_t)pixel_stride * height;
        break;
    case OCR_FMT_YUYV:
        if (width & 1U) return 0;
        stride_bytes = (uint64_t)pixel_stride * 2U;
        if (stride_bytes > UINT32_MAX) return 0;
        strides[0] = (uint32_t)stride_bytes;
        bytes = stride_bytes * height;
        break;
    case OCR_FMT_RGB565:
        stride_bytes = (uint64_t)pixel_stride * 2U;
        if (stride_bytes > UINT32_MAX) return 0;
        strides[0] = (uint32_t)stride_bytes;
        bytes = stride_bytes * height;
        break;
    case OCR_FMT_RGB888:
        stride_bytes = (uint64_t)pixel_stride * 3U;
        if (stride_bytes > UINT32_MAX) return 0;
        strides[0] = (uint32_t)stride_bytes;
        bytes = stride_bytes * height;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        stride_bytes = (uint64_t)pixel_stride * 4U;
        if (stride_bytes > UINT32_MAX) return 0;
        strides[0] = (uint32_t)stride_bytes;
        bytes = stride_bytes * height;
        break;
    default:
        return 0;
    }
    return bytes > SIZE_MAX ? 0 : (size_t)bytes;
}

static int alloc_from_path(const char *path, size_t size)
{
    int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1;

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int ret;
    do {
        ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data);
    } while (ret < 0 && errno == EINTR);
    if (ret != 0) {
        int saved_errno = errno;
        close(heap_fd);
        errno = saved_errno;
        return -1;
    }
    close(heap_fd);
    if (data.fd > INT_MAX) {
        (void)close((int)data.fd);
        errno = EOVERFLOW;
        return -1;
    }
    return (int)data.fd;
}

int ocr_dma_heap_alloc(const char *heap_path, size_t size)
{
    if (size == 0) return -1;
    if (heap_path && *heap_path) return alloc_from_path(heap_path, size);

    static const char *const candidates[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/system-uncached",
        "/dev/dma_heap/system-allocated",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int fd = alloc_from_path(candidates[i], size);
        if (fd >= 0) return fd;
    }
    LOG_E("no usable system DMA heap found: %s", strerror(errno));
    return -2;
}

int ocr_dma_heap_alloc_pool(ocr_buffer_pool_t *pool, int count,
                            uint32_t width, uint32_t height,
                            ocr_pixel_format_t format,
                            const char *heap_path, int map_cpu)
{
    if (!pool) return -1;
    uint32_t plane_count;
    uint32_t strides[4], offsets[4];
    size_t size = image_layout(width, height, format, &plane_count,
                               strides, offsets);
    if (size == 0 || ocr_pool_init(pool, count) != 0) return -1;

    for (int i = 0; i < count; ++i) {
        int fd = ocr_dma_heap_alloc(heap_path, size);
        if (fd < 0 || ocr_pool_register(pool, i, fd, size, width, height,
                                        format) != 0) {
            if (fd >= 0) close(fd);
            (void)ocr_pool_destroy(pool);
            return -2;
        }
        if (ocr_buffer_set_layout(&pool->buffers[i], plane_count,
                                  strides, offsets) != 0) {
            (void)ocr_pool_destroy(pool);
            return -2;
        }
        if (map_cpu && ocr_buffer_mmap(&pool->buffers[i]) != 0) {
            (void)ocr_pool_destroy(pool);
            return -3;
        }
    }
    return 0;
}
