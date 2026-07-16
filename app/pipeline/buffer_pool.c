/**
 * @file buffer_pool.c
 * @brief Fixed-capacity DMA-BUF pool implementation.
 */
#include "buffer_pool.h"
#include "log.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int pool_buffer_index(const ocr_buffer_pool_t *pool,
                             const ocr_buffer_t *buf)
{
    uint32_t index;

    if (!pool || !buf) {
        return -1;
    }
    index = buf->index;
    if (index >= (uint32_t)pool->count ||
        buf != &pool->buffers[index]) {
        return -1;
    }
    return (int)index;
}

static void pool_recycle_buffer(ocr_buffer_t *buf, void *user_data)
{
    ocr_buffer_pool_t *pool = (ocr_buffer_pool_t *)user_data;
    int index;
    int recycled = 0;
    int callback_failed = 0;

    if (!pool || !buf || !pool->initialized) {
        return;
    }

    ocr_mutex_lock(&pool->lock);
    index = pool_buffer_index(pool, buf);
    if (pool->initialized && index >= 0 &&
        pool->slot_state[index] == OCR_POOL_SLOT_IN_USE &&
        atomic_load_explicit(&buf->refcount, memory_order_acquire) == 0 &&
        pool->free_count < pool->registered_count) {
        /* Notify before publishing the FREE state. This serializes a fast
         * V4L2 QBUF/DQBUF cycle and pool destruction with the callback. */
        if (pool->recycle_cb &&
            pool->recycle_cb(buf, pool->recycle_user_data) != 0) {
            callback_failed = 1;
        }
        pool->slot_state[index] = OCR_POOL_SLOT_FREE;
        pool->free_list[pool->free_count++] = index;
        recycled = 1;
    }
    ocr_mutex_unlock(&pool->lock);

    if (!recycled) {
        LOG_E("buffer pool rejected an invalid recycle request");
    } else if (callback_failed) {
        LOG_W("buffer pool recycle notification failed for slot %u", buf->index);
    }
}

int ocr_pool_set_recycle_callback(ocr_buffer_pool_t *pool,
                                  ocr_pool_recycle_fn callback,
                                  void *user_data)
{
    if (!pool || !pool->initialized) return -1;
    ocr_mutex_lock(&pool->lock);
    if (pool->registered_count != 0 || pool->free_count != 0) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }
    pool->recycle_cb = callback;
    pool->recycle_user_data = user_data;
    ocr_mutex_unlock(&pool->lock);
    return 0;
}

int ocr_pool_is_idle(ocr_buffer_pool_t *pool)
{
    int idle;

    if (!pool || !pool->initialized) return -1;
    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized) {
        ocr_mutex_unlock(&pool->lock);
        return -1;
    }
    idle = pool->registered_count == pool->free_count;
    ocr_mutex_unlock(&pool->lock);
    return idle;
}

int ocr_pool_init(ocr_buffer_pool_t *pool, int count)
{
    if (!pool || count <= 0 || count > OCR_POOL_MAX_BUFS) {
        return -1;
    }

    memset(pool, 0, sizeof(*pool));
    if (ocr_mutex_init(&pool->lock) != 0) {
        return -2;
    }

    pool->count = count;
    for (int i = 0; i < count; ++i) {
        if (ocr_buffer_init(&pool->buffers[i]) != 0) {
            ocr_mutex_destroy(&pool->lock);
            memset(pool, 0, sizeof(*pool));
            return -2;
        }
        pool->buffers[i].index = (uint32_t)i;
        pool->buffers[i].owned = 1;
        ocr_buffer_set_release_callback(&pool->buffers[i],
                                        pool_recycle_buffer, pool);
        atomic_store_explicit(&pool->buffers[i].refcount, 0,
                              memory_order_release);
        pool->slot_state[i] = OCR_POOL_SLOT_UNUSED;
    }
    pool->initialized = 1;
    return 0;
}

int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height,
                      ocr_pixel_format_t format)
{
    if (!pool || !pool->initialized || index < 0 || index >= pool->count ||
        fd < 0 || size == 0 || width == 0 || height == 0 ||
        format <= OCR_FMT_UNKNOWN || format > OCR_FMT_BGRA8888 ||
        fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized ||
        pool->slot_state[index] != OCR_POOL_SLOT_UNUSED) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }

    for (int i = 0; i < pool->count; ++i) {
        if (pool->slot_state[i] != OCR_POOL_SLOT_UNUSED &&
            pool->buffers[i].fd == fd) {
            ocr_mutex_unlock(&pool->lock);
            return -2;
        }
    }

    uint32_t strides[4] = {0};
    uint32_t offsets[4] = {0};
    uint32_t planes = 1;
    uint64_t row_bytes = 0;
    uint64_t y_bytes = (uint64_t)width * height;
    uint64_t required = 0;
    switch (format) {
    case OCR_FMT_NV12:
        if ((width & 1U) || (height & 1U) || y_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        planes = 2;
        strides[0] = width;
        strides[1] = width;
        offsets[1] = (uint32_t)y_bytes;
        required = y_bytes + y_bytes / 2U;
        break;
    case OCR_FMT_NV16:
        if ((width & 1U) || y_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        planes = 2;
        strides[0] = width;
        strides[1] = width;
        offsets[1] = (uint32_t)y_bytes;
        required = y_bytes * 2U;
        break;
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:
        row_bytes = (uint64_t)width * 2U;
        required = row_bytes * height;
        break;
    case OCR_FMT_RGB888:
        row_bytes = (uint64_t)width * 3U;
        required = row_bytes * height;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        row_bytes = (uint64_t)width * 4U;
        required = row_bytes * height;
        break;
    default:
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }
    if (planes == 1) {
        if (row_bytes > UINT32_MAX) {
            ocr_mutex_unlock(&pool->lock);
            return -3;
        }
        strides[0] = (uint32_t)row_bytes;
    }
    if (required > size) {
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }

    ocr_buffer_t *buf = &pool->buffers[index];
    buf->fd = fd;
    buf->size = size;
    buf->width = width;
    buf->height = height;
    buf->format = format;
    if (ocr_buffer_set_layout(buf, planes, strides, offsets) != 0) {
        buf->fd = -1;
        buf->size = 0;
        ocr_mutex_unlock(&pool->lock);
        return -3;
    }
    pool->slot_state[index] = OCR_POOL_SLOT_FREE;
    pool->free_list[pool->free_count++] = index;
    ++pool->registered_count;
    ocr_mutex_unlock(&pool->lock);
    return 0;
}

ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool)
{
    ocr_buffer_t *buf;
    int index;

    if (!pool || !pool->initialized) {
        return NULL;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized || pool->free_count == 0) {
        ocr_mutex_unlock(&pool->lock);
        return NULL;
    }

    index = pool->free_list[--pool->free_count];
    if (index < 0 || index >= pool->count ||
        pool->slot_state[index] != OCR_POOL_SLOT_FREE) {
        ++pool->free_count;
        ocr_mutex_unlock(&pool->lock);
        LOG_E("buffer pool free-list corruption detected");
        return NULL;
    }

    buf = &pool->buffers[index];
    pool->slot_state[index] = OCR_POOL_SLOT_IN_USE;
    buf->timestamp = 0;
    buf->frame_id = 0;
    atomic_store_explicit(&buf->refcount, 1, memory_order_release);
    ocr_mutex_unlock(&pool->lock);
    return buf;
}

ocr_buffer_t *ocr_pool_acquire_index(ocr_buffer_pool_t *pool, int index)
{
    ocr_buffer_t *buf;
    int free_pos = -1;

    if (!pool || !pool->initialized || index < 0 || index >= pool->count) {
        return NULL;
    }

    ocr_mutex_lock(&pool->lock);
    if (!pool->initialized ||
        pool->slot_state[index] != OCR_POOL_SLOT_FREE) {
        ocr_mutex_unlock(&pool->lock);
        return NULL;
    }
    for (int i = 0; i < pool->free_count; ++i) {
        if (pool->free_list[i] == index) {
            free_pos = i;
            break;
        }
    }
    if (free_pos < 0) {
        ocr_mutex_unlock(&pool->lock);
        LOG_E("buffer pool free-list corruption detected for slot %d", index);
        return NULL;
    }

    pool->free_list[free_pos] = pool->free_list[--pool->free_count];
    pool->slot_state[index] = OCR_POOL_SLOT_IN_USE;
    buf = &pool->buffers[index];
    buf->timestamp = 0;
    buf->frame_id = 0;
    atomic_store_explicit(&buf->refcount, 1, memory_order_release);
    ocr_mutex_unlock(&pool->lock);
    return buf;
}

int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf)
{
    int index;
    int valid;

    if (!pool || !buf || !pool->initialized) {
        return -1;
    }

    ocr_mutex_lock(&pool->lock);
    index = pool_buffer_index(pool, buf);
    valid = pool->initialized && index >= 0 &&
            pool->slot_state[index] == OCR_POOL_SLOT_IN_USE &&
            buf->release_cb == pool_recycle_buffer &&
            buf->release_user_data == pool;
    ocr_mutex_unlock(&pool->lock);

    if (!valid || ocr_buffer_unref(buf) < 0) {
        return -2;
    }
    return 0;
}

int ocr_pool_destroy(ocr_buffer_pool_t *pool)
{
    if (!pool || !pool->initialized) {
        return 0;
    }

    ocr_mutex_lock(&pool->lock);
    if (pool->registered_count != pool->free_count) {
        ocr_mutex_unlock(&pool->lock);
        return -2;
    }

    pool->initialized = 0;
    for (int i = 0; i < pool->count; ++i) {
        ocr_buffer_t *buf = &pool->buffers[i];

        ocr_buffer_set_release_callback(buf, NULL, NULL);
        ocr_buffer_munmap(buf);
        if (buf->fd >= 0) {
            (void)close(buf->fd);
            buf->fd = -1;
        }
        buf->owned = 0;
        pool->slot_state[i] = OCR_POOL_SLOT_UNUSED;
    }
    pool->count = 0;
    pool->registered_count = 0;
    pool->free_count = 0;
    pool->recycle_cb = NULL;
    pool->recycle_user_data = NULL;
    ocr_mutex_unlock(&pool->lock);
    ocr_mutex_destroy(&pool->lock);
    return 0;
}
