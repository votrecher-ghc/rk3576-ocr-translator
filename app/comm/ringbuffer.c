/**
 * @file ringbuffer.c
 * @brief 无锁 SPSC 环形缓冲实现
 */
#include "ringbuffer.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

int ocr_ringbuffer_init(ocr_ringbuffer_t *rb, size_t elem_size, size_t count)
{
    if (!rb || elem_size == 0 || count == 0) {
        return -1;
    }

    /* 建议 count 为 2 的幂，但此处不强制 */
    rb->elem_size = elem_size;
    rb->count     = count;
    rb->capacity  = elem_size * count;
    rb->buf       = (uint8_t *)malloc(rb->capacity);
    if (!rb->buf) {
        LOG_E("ringbuffer 内存分配失败");
        return -2;
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
}

int ocr_ringbuffer_push(ocr_ringbuffer_t *rb, const void *elem)
{
    if (!rb || !elem) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    size_t next = (head + 1) % rb->count;
    if (next == tail) {
        /* 缓冲已满，丢弃 */
        return 1;
    }

    memcpy(rb->buf + head * rb->elem_size, elem, rb->elem_size);
    atomic_store_explicit(&rb->head, next, memory_order_release);
    return 0;
}

int ocr_ringbuffer_pop(ocr_ringbuffer_t *rb, void *elem)
{
    if (!rb || !elem) return -1;

    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);

    if (tail == head) {
        /* 空 */
        return 1;
    }

    memcpy(elem, rb->buf + tail * rb->elem_size, rb->elem_size);
    atomic_store_explicit(&rb->tail, (tail + 1) % rb->count, memory_order_release);
    return 0;
}

size_t ocr_ringbuffer_size(const ocr_ringbuffer_t *rb)
{
    if (!rb) return 0;
    size_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    return (head + rb->count - tail) % rb->count;
}
