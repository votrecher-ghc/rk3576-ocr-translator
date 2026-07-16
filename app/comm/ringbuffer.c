/**
 * @file ringbuffer.c
 * @brief 无锁 SPSC 环形缓冲实现
 */
#include "ringbuffer.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

int ocr_ringbuffer_init(ocr_ringbuffer_t *rb, size_t elem_size, size_t count)
{
    if (!rb || elem_size == 0 || count == 0) {
        return -1;
    }

    if (elem_size > SIZE_MAX / count) {
        return -2;
    }

    memset(rb, 0, sizeof(*rb));
    rb->elem_size = elem_size;
    rb->count     = count;
    rb->mask      = (count & (count - 1)) == 0 ? count - 1 : 0;
    rb->capacity  = elem_size * count;
    rb->buf       = (uint8_t *)calloc(count, elem_size);
    if (!rb->buf) {
        LOG_E("ringbuffer 内存分配失败");
        return -3;
    }

    atomic_store(&rb->head, 0);
    atomic_store(&rb->tail, 0);
    return 0;
}

void ocr_ringbuffer_destroy(ocr_ringbuffer_t *rb)
{
    if (!rb) return;
    free(rb->buf);
    rb->buf = NULL;
    rb->capacity = 0;
    rb->elem_size = 0;
    rb->count = 0;
    rb->mask = 0;
    atomic_store_explicit(&rb->head, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0, memory_order_relaxed);
}

static size_t rb_index(const ocr_ringbuffer_t *rb, size_t pos)
{
    return rb->mask ? (pos & rb->mask) : (pos % rb->count);
}

int ocr_ringbuffer_push(ocr_ringbuffer_t *rb, const void *elem)
{
    if (!rb || !elem || !rb->buf || rb->count == 0) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head - tail >= rb->count) {
        /* 缓冲已满，丢弃 */
        return 1;
    }

    memcpy(rb->buf + rb_index(rb, head) * rb->elem_size, elem, rb->elem_size);
    atomic_store_explicit(&rb->head, head + 1, memory_order_release);
    return 0;
}

int ocr_ringbuffer_pop(ocr_ringbuffer_t *rb, void *elem)
{
    if (!rb || !elem || !rb->buf || rb->count == 0) return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (tail == head) {
        /* 空 */
        return 1;
    }

    memcpy(elem, rb->buf + rb_index(rb, tail) * rb->elem_size, rb->elem_size);
    atomic_store_explicit(&rb->tail, tail + 1, memory_order_release);
    return 0;
}

size_t ocr_ringbuffer_size(const ocr_ringbuffer_t *rb)
{
    if (!rb || !rb->buf || rb->count == 0) return 0;
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    size_t size = head - tail;
    return size <= rb->count ? size : rb->count;
}
