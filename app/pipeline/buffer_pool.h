#ifndef OCR_PIPELINE_BUFFER_POOL_H
#define OCR_PIPELINE_BUFFER_POOL_H
/**
 * @file buffer_pool.h
 * @brief 缓冲池管理：预分配/分配/回收/引用计数
 *
 * 预分配一组 DMA-BUF 缓冲区，通过引用计数管理生命周期，
 * 避免运行时频繁分配释放。
 */

#include "buffer.h"
#include "thread.h"

#define OCR_POOL_MAX_BUFS 32

/** 缓冲池 */
typedef struct {
    ocr_mutex_t   lock;
    ocr_buffer_t  buffers[OCR_POOL_MAX_BUFS]; /* 预分配缓冲数组 */
    int           count;        /* 缓冲区个数 */
    int           free_count;   /* 当前可用个数 */
    int           free_list[OCR_POOL_MAX_BUFS]; /* 空闲索引栈 */
} ocr_buffer_pool_t;

/**
 * @brief 初始化缓冲池（仅初始化元数据，不分配 DMA-BUF）
 * @param[in] pool   缓冲池
 * @param[in] count  缓冲区个数
 * @return 0=成功，负数=错误
 */
int ocr_pool_init(ocr_buffer_pool_t *pool, int count);

/**
 * @brief 注册已分配的 DMA-BUF 到缓冲池槽位
 * @param[in] pool   缓冲池
 * @param[in] index  槽位索引
 * @param[in] fd     DMA-BUF fd
 * @param[in] size   缓冲区大小
 * @param[in] width  像素宽
 * @param[in] height 像素高
 * @param[in] format 像素格式
 * @return 0=成功，负数=错误
 */
int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height, ocr_pixel_format_t format);

/**
 * @brief 从池中获取一个空闲缓冲区（引用计数=1）
 * @return 缓冲区指针，NULL=无可用
 */
ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool);

/**
 * @brief 归还缓冲区到池（引用计数归零时调用）
 */
int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf);

/**
 * @brief 销毁缓冲池（关闭 fd，释放资源）
 */
void ocr_pool_destroy(ocr_buffer_pool_t *pool);

#endif /* OCR_PIPELINE_BUFFER_POOL_H */
