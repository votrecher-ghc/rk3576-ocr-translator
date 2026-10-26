/**
 * @file ringbuffer.c
 * @brief 无锁 SPSC 环形缓冲实现
 *
 * 固定元素大小的单生产者/单消费者队列；push/pop 拷贝元素内容，不接管元素内指针的资源。
 * 生产者先写数据再 release 发布 head，消费者 acquire 观察 head 后读取；容量满时返回 1。
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

/** @brief 逻辑序号以 size_t 递增；容量为二次幂时用位掩码，否则用取模映射到存储槽。 */
static size_t rb_index(const ocr_ringbuffer_t *rb, size_t pos)
{
    return rb->mask ? (pos & rb->mask) : (pos % rb->count);
}

/** @brief 生产者先确认空位，复制元素后 release 发布 head，不能被多个生产者并发调用。 */
int ocr_ringbuffer_push(ocr_ringbuffer_t *rb, const void *elem)
{
    if (!rb || !elem || !rb->buf || rb->count == 0) return -1;

    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);

    if (head - tail >= rb->count) {
        /* 缓冲已满，丢弃 */
        return 1;
    }

    /* 先写槽内数据，再 release 发布 head；消费者 acquire 看到新 head 后，
     * 才能保证也看见该槽的数据。只把 head 声明 volatile 不能提供这种顺序。 */
    memcpy(rb->buf + rb_index(rb, head) * rb->elem_size, elem, rb->elem_size);
    atomic_store_explicit(&rb->head, head + 1, memory_order_release);
    return 0;
}

/** @brief 消费者 acquire 观察 head 后读取元素，再发布 tail 允许生产者复用该槽。 */
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
    /* 复制完成才允许生产者复用该槽，生产者 acquire 读取 tail 与此配对。
     * elem 内若有指针，memcpy 只复制地址，不增加对应对象的任何引用。 */
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
