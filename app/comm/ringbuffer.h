#ifndef OCR_COMM_RINGBUFFER_H
#define OCR_COMM_RINGBUFFER_H
/**
 * @file ringbuffer.h
 * @brief 无锁 SPSC（单生产者-单消费者）环形缓冲
 *
 * 用于 IMU 等高频数据的单生产者-单消费者场景，
 * 基于 atomic 原子操作实现无锁，避免锁开销。
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

/** SPSC 无锁环形缓冲 */
typedef struct {
    uint8_t        *buf;        /* 底层缓冲区 */
    size_t          capacity;   /* 总字节容量 */
    size_t          elem_size;  /* 单元素字节大小 */
    size_t          count;      /* 元素个数（=capacity/elem_size） */
    atomic_size_t   head;       /* 生产者写位置 */
    atomic_size_t   tail;       /* 消费者读位置 */
} ocr_ringbuffer_t;

/**
 * @brief 初始化环形缓冲
 * @param[in] rb        环形缓冲句柄
 * @param[in] elem_size 单元素字节数
 * @param[in] count     元素个数（必须为 2 的幂以加速取模）
 * @return 0=成功，负数=错误
 */
int ocr_ringbuffer_init(ocr_ringbuffer_t *rb, size_t elem_size, size_t count);

/**
 * @brief 销毁环形缓冲
 */
void ocr_ringbuffer_destroy(ocr_ringbuffer_t *rb);

/**
 * @brief 向缓冲写入一个元素（生产者）
 * @return 0=成功，1=已满丢弃，负数=错误
 */
int ocr_ringbuffer_push(ocr_ringbuffer_t *rb, const void *elem);

/**
 * @brief 从缓冲读取一个元素（消费者）
 * @return 0=成功，1=空，负数=错误
 */
int ocr_ringbuffer_pop(ocr_ringbuffer_t *rb, void *elem);

/**
 * @brief 查询当前已用元素数
 */
size_t ocr_ringbuffer_size(const ocr_ringbuffer_t *rb);

/**
 * @brief 查询是否为空
 */
static inline int ocr_ringbuffer_empty(const ocr_ringbuffer_t *rb) {
    return ocr_ringbuffer_size(rb) == 0;
}

#endif /* OCR_COMM_RINGBUFFER_H */
