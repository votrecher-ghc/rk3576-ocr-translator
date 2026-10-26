/** @file dma_heap.c @brief Linux DMA-HEAP allocation implementation.
 *
 * 从 Linux DMA-HEAP 分配共享图像内存，按 16 像素对齐计算行跨度和各平面偏移。
 * heap fd 用完即关闭，分配得到的 DMA-BUF fd 转交缓冲池；map_cpu 只决定是否额外建立 CPU 映射。
 */
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

/** @brief 按像素格式计算对齐后总大小和逻辑平面布局，同时检查乘加溢出。 */
static size_t image_layout(uint32_t width, uint32_t height,
                           ocr_pixel_format_t format,
                           uint32_t *plane_count,
                           uint32_t strides[4], uint32_t offsets[4])
{
    if (width == 0 || height == 0 || !plane_count || !strides || !offsets ||
        width > UINT32_MAX - (OCR_DMA_STRIDE_ALIGN - 1U)) return 0;
    memset(strides, 0, sizeof(uint32_t) * 4U);
    memset(offsets, 0, sizeof(uint32_t) * 4U);
    /* 向上对齐到 16 像素：例如 width=642 -> pixel_stride=656。
     * width 保持有效宽度 642，只有分配行跨度变成 656；前面的上界检查防止 +15 溢出。
     * 这个位运算要求对齐值是二次幂，不能随意把常量改成任意整数。 */
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
        /* NV12: Y 大小=stride*height，UV 紧接 Y 且只有 height/2 行。
         * 平面 1 的 offset 是 Y 的分配大小，不是另一块 DMA-BUF 的地址。 */
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
        /* RGB888 每像素三个字节：先对齐像素宽，再乘 3 得到字节 stride。
         * RGA 包装时会除以 3 还原 wstride，DRM/CPU 则直接用字节 stride。 */
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

/** @brief 打开指定 heap 并分配 DMA-BUF；关闭 heap 句柄后分配出的 fd 仍独立有效。 */
static int alloc_from_path(const char *path, size_t size)
{
    int heap_fd = open(path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) return -1;

    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    /* heap_fd 是分配器设备；ioctl 成功后 data.fd 是新图像内存的描述符。
     * 两者是不同文件对象，因此关闭 heap_fd 不会释放新分配的 DMA-BUF。 */
    data.len = size;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int ret;
    do {
        ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data);
    } while (ret < 0 && errno == EINTR);
    if (ret != 0) {
        /* close 也可能修改 errno，先保存分配失败原因，让上层日志能定位真问题。 */
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

/** @brief 指定路径时只尝试该 heap；未指定则按候选顺序尝试，返回 fd 或负数错误。 */
int ocr_dma_heap_alloc(const char *heap_path, size_t size)
{
    if (size == 0) return -1;
    if (heap_path && *heap_path) return alloc_from_path(heap_path, size);

    /* 顺序表示优先级：system 可用就立即返回，不会额外尝试 uncached。
     * heap 名称由板端内核决定；这里的成功仅证明分配成功，不验证设备导入能力。 */
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

/** @brief 一次性创建固定数量同布局缓冲，部分失败时销毁已注册缓冲并关闭 fd。 */
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
        /* register 的默认紧密布局只是初值，此处覆盖为按 16 像素对齐的真实布局。
         * 若任一槽失败，已成功注册的 fd 全由 destroy 归还，不留下半个可用池。 */
        if (ocr_buffer_set_layout(&pool->buffers[i], plane_count,
                                  strides, offsets) != 0) {
            (void)ocr_pool_destroy(pool);
            return -2;
        }
        /* 纯 RGA->DRM 可只传 fd；当前 OCR 在 CPU 上预处理，调用方传 map_cpu=1。
         * 建立映射本身不读取图像；实际 CPU 访问仍需要 START/END 缓存同步。 */
        if (map_cpu && ocr_buffer_mmap(&pool->buffers[i]) != 0) {
            (void)ocr_pool_destroy(pool);
            return -3;
        }
    }
    return 0;
}
