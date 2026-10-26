#ifndef OCR_PIPELINE_BUFFER_POOL_H
#define OCR_PIPELINE_BUFFER_POOL_H

/**
 * @file buffer_pool.h
 * @brief Fixed-capacity, reference-counted DMA-BUF pool.
 *
 * 缓冲池只管理已注册 fd 的所有权和使用状态，不在 init 中分配图像内存。
 * acquire 得到一个引用，最后一次 unref 才回收；采集池还通过回收回调把缓冲重新交给摄像头。
 */

#include "buffer.h"
#include "thread.h"

#include <stdint.h>

#define OCR_POOL_MAX_BUFS 32

typedef enum {
    OCR_POOL_SLOT_UNUSED = 0, /* 尚未注册 fd，不可分配。 */
    OCR_POOL_SLOT_FREE,       /* 没有应用引用；采集池中不等于摄像头已经写完。 */
    OCR_POOL_SLOT_IN_USE,     /* 已被 acquire，等待最后一个引用归还。 */
} ocr_pool_slot_state_t;

/**
 * @brief 最后一份应用引用归还槽位时的可选通知
 * @return 0=通知成功，非零=失败；池记录失败，但仍将槽位发布为 FREE
 * @note 在最终 unref 的线程、池锁内同步执行，不能重入池接口；buf 仅借用。
 *       设备回收失败的业务恢复由回调所属模块负责，FREE 不证明设备已经接收缓冲。
 */
typedef int (*ocr_pool_recycle_fn)(ocr_buffer_t *buf, void *user_data);

typedef struct {
    ocr_mutex_t   lock;              /* 保护槽位状态、空闲列表及回收通知顺序。 */
    ocr_buffer_t  buffers[OCR_POOL_MAX_BUFS]; /* 元数据地址在池生命周期内保持稳定。 */
    int           count;             /* init 指定的槽位容量，1..OCR_POOL_MAX_BUFS；不等于已注册数量。 */
    int           registered_count;  /* 已接管有效 fd 的槽位数；注册成功递增，销毁时清零。 */
    int           free_count;        /* free_list 前段有效元素个数，受 lock 保护；取用减一、回收加一。 */
    int           free_list[OCR_POOL_MAX_BUFS]; /* 空闲槽号栈，前 free_count 项有效；不是摄像头完成队列。 */
    uint8_t       slot_state[OCR_POOL_MAX_BUFS]; /* 每槽 UNUSED/FREE/IN_USE 状态，受 lock 保护，防止重复取用/回收。 */
    ocr_pool_recycle_fn recycle_cb;   /* 最后一次 unref 归还槽位时在池锁内调用；采集池借此 QBUF，不可重入池接口。 */
    void         *recycle_user_data; /**< 借给 recycle_cb 的上下文，如采集器 cap；须活到全部回收完成。 */
    int           initialized; /**< 池锁和槽位状态已初始化；不是“已分配全部 DMA 内存”的标志。 */
} ocr_buffer_pool_t;

/** @brief 只初始化管理结构；实际内存由 V4L2 或 DMA-HEAP 分配后再注册。 */
int ocr_pool_init(ocr_buffer_pool_t *pool, int count);

/**
 * Register a DMA-BUF and transfer ownership of fd to the pool.
 * A slot can be registered once between init and destroy.
 *
 * 把 fd 所有权转交池并设置初始布局；失败时 fd 仍由调用者负责关闭。
 */
int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height,
                      ocr_pixel_format_t format);

/**
 * @brief 在注册任何 fd 前设置整池的回收通知。
 * @note 回调运行时已经持有池锁，不能再次调用池接口，否则可能自锁。
 */
int ocr_pool_set_recycle_callback(ocr_buffer_pool_t *pool,
                                  ocr_pool_recycle_fn callback,
                                  void *user_data);

/** Return 1 when every registered slot is free, 0 when busy, or <0 on error. */
int ocr_pool_is_idle(ocr_buffer_pool_t *pool);

/** Acquire one reference to a registered free buffer, or NULL if exhausted.
 *
 * 从空闲列表取一块缓冲并赋予一个引用；池耗尽时返回 NULL，不等待。
 */
ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool);

/** Acquire the exact registered slot (for APIs such as V4L2 DQBUF).
 *
 * 认领 DQBUF 指定的槽位，防止拿到与摄像头实际完成帧不同的缓冲。
 */
ocr_buffer_t *ocr_pool_acquire_index(ocr_buffer_pool_t *pool, int index);

/**
 * Release one caller-owned reference. The buffer is recycled only when its
 * reference count reaches zero. Duplicate release is rejected.
 *
 * 确认缓冲属于此池后释放一个引用；真正回收由最后一次 unref 触发。
 */
int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf);

/**
 * Close registered fds and destroy the pool. Returns -2 while buffers are in
 * use, leaving the pool intact so callers can release them and retry.
 * Repeated calls after a successful destroy return 0.
 *
 * 仅在所有缓冲都空闲时解除映射并关闭 fd；忙碌时返回 -2，保留池供重试。
 */
int ocr_pool_destroy(ocr_buffer_pool_t *pool);

#endif /* OCR_PIPELINE_BUFFER_POOL_H */
