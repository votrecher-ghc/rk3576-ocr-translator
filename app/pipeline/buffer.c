/**
 * @file buffer.c
 * @brief DMA-BUF buffer metadata and reference counting.
 */
#include "buffer.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/mman.h>

int ocr_buffer_init(ocr_buffer_t *buf)
{
    if (!buf) {
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    buf->fd = -1;
    buf->format = OCR_FMT_UNKNOWN;
    atomic_init(&buf->refcount, 1);
    return 0;
}

int ocr_buffer_set_release_callback(ocr_buffer_t *buf,
                                    ocr_buffer_release_fn release_cb,
                                    void *user_data)
{
    if (!buf) {
        return -1;
    }

    /* This API is intentionally setup-only; callers provide exclusivity. */
    buf->release_cb = release_cb;
    buf->release_user_data = user_data;
    return 0;
}

int ocr_buffer_set_layout(ocr_buffer_t *buf, uint32_t plane_count,
                          const uint32_t *strides, const uint32_t *offsets)
{
    uint32_t expected_planes;
    uint64_t row_bytes[4] = {0};
    uint64_t rows[4] = {0};
    uint64_t previous_end = 0;

    if (!buf || !strides || !offsets || plane_count == 0 || plane_count > 4 ||
        buf->size == 0 || buf->width == 0 || buf->height == 0)
        return -1;

    switch (buf->format) {
    case OCR_FMT_NV12:
        if ((buf->width & 1U) || (buf->height & 1U)) return -2;
        expected_planes = 2;
        row_bytes[0] = buf->width;
        row_bytes[1] = buf->width;
        rows[0] = buf->height;
        rows[1] = buf->height / 2U;
        break;
    case OCR_FMT_NV16:
        if (buf->width & 1U) return -2;
        expected_planes = 2;
        row_bytes[0] = buf->width;
        row_bytes[1] = buf->width;
        rows[0] = buf->height;
        rows[1] = buf->height;
        break;
    case OCR_FMT_YUYV:
        if (buf->width & 1U) return -2;
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 2U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_RGB565:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 2U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_RGB888:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 3U;
        rows[0] = buf->height;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        expected_planes = 1;
        row_bytes[0] = (uint64_t)buf->width * 4U;
        rows[0] = buf->height;
        break;
    default:
        return -2;
    }
    if (plane_count != expected_planes) return -2;

    for (uint32_t i = 0; i < plane_count; ++i) {
        uint64_t end;
        uint64_t allocation_end;

        if ((uint64_t)strides[i] < row_bytes[i] || offsets[i] >= buf->size ||
            (i > 0 && offsets[i] < previous_end)) {
            return -2;
        }
        end = (uint64_t)offsets[i] +
              (uint64_t)strides[i] * (rows[i] - 1U) + row_bytes[i];
        allocation_end = (uint64_t)offsets[i] +
                         (uint64_t)strides[i] * rows[i];
        if (end > buf->size || allocation_end > buf->size) return -2;
        previous_end = allocation_end;
    }
    buf->plane_count = plane_count;
    memcpy(buf->strides, strides, plane_count * sizeof(strides[0]));
    memcpy(buf->offsets, offsets, plane_count * sizeof(offsets[0]));
    for (uint32_t i = plane_count; i < 4; ++i) {
        buf->strides[i] = 0;
        buf->offsets[i] = 0;
    }
    return 0;
}

int ocr_buffer_ref(ocr_buffer_t *buf)
{
    int current;

    if (!buf) {
        return -1;
    }

    current = atomic_load_explicit(&buf->refcount, memory_order_acquire);
    for (;;) {
        if (current <= 0 || current == INT_MAX) {
            return -1;
        }
        if (atomic_compare_exchange_weak_explicit(&buf->refcount,
                                                  &current,
                                                  current + 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return current + 1;
        }
    }
}

int ocr_buffer_unref(ocr_buffer_t *buf)
{
    int current;
    int remaining;

    if (!buf) {
        return -1;
    }

    current = atomic_load_explicit(&buf->refcount, memory_order_acquire);
    for (;;) {
        if (current <= 0) {
            return -1;
        }
        remaining = current - 1;
        if (atomic_compare_exchange_weak_explicit(&buf->refcount,
                                                  &current,
                                                  remaining,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            break;
        }
    }

    if (remaining == 0 && buf->release_cb) {
        buf->release_cb(buf, buf->release_user_data);
    }
    return remaining;
}

int ocr_buffer_mmap(ocr_buffer_t *buf)
{
    void *addr;

    if (!buf || buf->fd < 0 || buf->size == 0) {
        return -1;
    }
    if (buf->mmap_addr) {
        return 0;
    }

    addr = mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                buf->fd, 0);
    if (addr == MAP_FAILED) {
        LOG_E("mmap fd=%d size=%zu failed: %s",
              buf->fd, buf->size, strerror(errno));
        return -2;
    }
    buf->mmap_addr = addr;
    return 0;
}

void ocr_buffer_munmap(ocr_buffer_t *buf)
{
    if (!buf || !buf->mmap_addr) {
        return;
    }

    if (buf->size > 0) {
        (void)munmap(buf->mmap_addr, buf->size);
    }
    buf->mmap_addr = NULL;
}

const char *ocr_pixel_format_str(ocr_pixel_format_t fmt)
{
    switch (fmt) {
    case OCR_FMT_NV12:     return "NV12";
    case OCR_FMT_NV16:     return "NV16";
    case OCR_FMT_YUYV:     return "YUYV";
    case OCR_FMT_RGB565:   return "RGB565";
    case OCR_FMT_RGB888:   return "RGB888";
    case OCR_FMT_ARGB8888: return "ARGB8888";
    case OCR_FMT_BGRA8888: return "BGRA8888";
    default:               return "UNKNOWN";
    }
}
