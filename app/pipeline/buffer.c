/**
 * @file buffer.c
 * @brief DMA-BUF 缓冲封装实现
 */
#include "buffer.h"
#include "log.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <atomic>

int ocr_buffer_init(ocr_buffer_t *buf)
{
    if (!buf) return -1;
    memset(buf, 0, sizeof(*buf));
    buf->fd = -1;
    buf->mmap_addr = NULL;
    buf->size = 0;
    buf->format = OCR_FMT_UNKNOWN;
    atomic_init(&buf->refcount, 1);
    return 0;
}

int ocr_buffer_ref(ocr_buffer_t *buf)
{
    if (!buf) return 0;
    return atomic_fetch_add(&buf->refcount, 1) + 1;
}

int ocr_buffer_unref(ocr_buffer_t *buf)
{
    if (!buf) return 0;
    int prev = atomic_fetch_sub(&buf->refcount, 1);
    int curr = prev - 1;
    if (curr <= 0) {
        /* 引用归零：解除映射，关闭 fd（由缓冲池决定是否复用） */
        ocr_buffer_munmap(buf);
        if (buf->fd >= 0 && buf->owned) {
            /* TODO: 归还到缓冲池，而非直接 close */
        }
    }
    return curr;
}

int ocr_buffer_mmap(ocr_buffer_t *buf)
{
    if (!buf || buf->fd < 0) return -1;
    if (buf->mmap_addr) return 0; /* 已映射 */

    void *addr = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      buf->fd, 0);
    if (addr == MAP_FAILED) {
        LOG_E("mmap fd=%d size=%zu 失败: %s", buf->fd, buf->size, strerror(errno));
        return -2;
    }
    buf->mmap_addr = addr;
    return 0;
}

void ocr_buffer_munmap(ocr_buffer_t *buf)
{
    if (!buf || !buf->mmap_addr) return;
    munmap(buf->mmap_addr, buf->size);
    buf->mmap_addr = NULL;
}

const char *ocr_pixel_format_str(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:      return "NV12";
        case OCR_FMT_NV16:      return "NV16";
        case OCR_FMT_YUYV:      return "YUYV";
        case OCR_FMT_RGB565:    return "RGB565";
        case OCR_FMT_RGB888:    return "RGB888";
        case OCR_FMT_ARGB8888:  return "ARGB8888";
        case OCR_FMT_BGRA8888:  return "BGRA8888";
        default:                return "UNKNOWN";
    }
}
