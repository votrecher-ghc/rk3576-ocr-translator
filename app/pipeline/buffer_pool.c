/**
 * @file buffer_pool.c
 * @brief 缓冲池管理实现
 */
#include "buffer_pool.h"
#include "log.h"

#include <string.h>
#include <unistd.h>

int ocr_pool_init(ocr_buffer_pool_t *pool, int count)
{
    if (!pool || count <= 0 || count > OCR_POOL_MAX_BUFS) {
        return -1;
    }
    memset(pool, 0, sizeof(*pool));
    pool->count = count;
    pool->free_count = count;

    /* 初始全部空闲 */
    for (int i = 0; i < count; i++) {
        ocr_buffer_init(&pool->buffers[i]);
        pool->buffers[i].index = i;
        pool->buffers[i].owned = 1;
        pool->free_list[i] = i;
    }
    ocr_mutex_init(&pool->lock);
    return 0;
}

int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height, ocr_pixel_format_t format)
{
    if (!pool || index < 0 || index >= pool->count) return -1;
    ocr_buffer_t *buf = &pool->buffers[index];
    buf->fd = fd;
    buf->size = size;
    buf->width = width;
    buf->height = height;
    buf->format = format;
    return 0;
}

ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool)
{
    if (!pool) return NULL;
    ocr_mutex_lock(&pool->lock);
    if (pool->free_count <= 0) {
        ocr_mutex_unlock(&pool->lock);
        LOG_W("缓冲池耗尽（count=%d）", pool->count);
        return NULL;
    }
    int idx = pool->free_list[--pool->free_count];
    ocr_buffer_t *buf = &pool->buffers[idx];
    atomic_store(&buf->refcount, 1);
    ocr_mutex_unlock(&pool->lock);
    return buf;
}

int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf)
{
    if (!pool || !buf) return -1;
    ocr_mutex_lock(&pool->lock);
    /* 将缓冲区索引压回空闲栈 */
    if (pool->free_count < pool->count) {
        pool->free_list[pool->free_count++] = buf->index;
    }
    ocr_mutex_unlock(&pool->lock);
    return 0;
}

void ocr_pool_destroy(ocr_buffer_pool_t *pool)
{
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) {
        ocr_buffer_t *buf = &pool->buffers[i];
        ocr_buffer_munmap(buf);
        if (buf->fd >= 0) {
            close(buf->fd);
            buf->fd = -1;
        }
    }
    ocr_mutex_destroy(&pool->lock);
    memset(pool, 0, sizeof(*pool));
}
