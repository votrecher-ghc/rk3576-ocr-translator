#ifndef OCR_COMM_RINGBUFFER_H
#define OCR_COMM_RINGBUFFER_H
/**
 * @file ringbuffer.h
 * @brief 无锁 SPSC（单生产者-单消费者）环形缓冲
 *
 * 用于 IMU 等高频数据的单生产者-单消费者场景，
 * 基于 atomic 原子操作实现无锁，避免锁开销。
 *
 * 固定元素大小的单生产者/单消费者队列；push/pop 拷贝元素内容，不接管元素内指针的资源。
 * 生产者先写数据再 release 发布 head，消费者 acquire 观察 head 后读取；容量满时返回 1。
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stddef.h>

/** SPSC 无锁环形缓冲 */
typedef struct {
    uint8_t        *buf; /**< 本队列 calloc 的连续元素存储，destroy 释放；只含值拷贝，不接管元素外部资源。 */
    size_t          capacity; /**< 底层存储总字节数，等于 elem_size*count；与可容纳元素数 count 区分。 */
    size_t          elem_size; /**< 每个元素的固定字节数，例如 sizeof(imu_sample_t)，初始化后不变。 */
    size_t          count; /**< 队列最大元素数，所有槽都可用；当前排队量由 head-tail 推算。 */
    size_t          mask; /**< count 为二次幂时为 count-1，用于快速索引；否则为 0，使用取模。 */
    atomic_size_t   head; /**< 单生产者发布的逻辑写序号；写完元素后 release 更新，不是已取模的数组索引。 */
    atomic_size_t   tail; /**< 单消费者发布的逻辑读序号；复制完元素后 release 更新，允许生产者复用该槽。 */
} ocr_ringbuffer_t;

/**
 * @brief 初始化环形缓冲
 * @param[in] rb        环形缓冲句柄
 * @param[in] elem_size 单元素字节数
 * @param[in] count     最大元素数，必须大于 0；为 2 的幂时使用位掩码加速，否则取模
 * @return 0=成功，负数=错误
 * @note 初始化不接管外部资源；重新初始化前须停止生产/消费并销毁旧队列。
 */
int ocr_ringbuffer_init(ocr_ringbuffer_t *rb, size_t elem_size, size_t count);

/**
 * @brief 销毁环形缓冲
 * @param[in,out] rb 待销毁的队列，允许为 NULL
 * @note 调用前必须结束所有 push/pop/size 访问；未出队元素中的指针不自动释放。
 */
void ocr_ringbuffer_destroy(ocr_ringbuffer_t *rb);

/**
 * @brief 向缓冲写入一个元素（生产者）
 * @param[in,out] rb 已初始化的队列；仅允许一个生产者调用
 * @param[in] elem 至少 elem_size 字节的源数据，返回前按值复制
 * @return 0=成功，1=已满且本次元素未写入，负数=错误
 * @note 满时保留原有元素；元素中若含指针，其指向对象的生命周期由业务层管理。
 */
int ocr_ringbuffer_push(ocr_ringbuffer_t *rb, const void *elem);

/**
 * @brief 从缓冲读取一个元素（消费者）
 * @param[in,out] rb 已初始化的队列；仅允许一个消费者调用
 * @param[out] elem 至少 elem_size 字节的目标空间，成功时写入一个元素副本
 * @return 0=成功，1=空，负数=错误
 */
int ocr_ringbuffer_pop(ocr_ringbuffer_t *rb, void *elem);

/**
 * @brief 查询当前已用元素数
 * @return 观察到的元素数，范围为 0..count；无效队列返回 0
 * @note 并发读写时仅作瞬时统计，不能替代 push/pop 自身的满/空判断。
 */
size_t ocr_ringbuffer_size(const ocr_ringbuffer_t *rb);

/**
 * @brief 查询是否为空
 */
static inline int ocr_ringbuffer_empty(const ocr_ringbuffer_t *rb) {
    return ocr_ringbuffer_size(rb) == 0;
}

#endif /* OCR_COMM_RINGBUFFER_H */
