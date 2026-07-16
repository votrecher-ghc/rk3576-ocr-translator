#ifndef OCR_PIPELINE_BUFFER_POOL_H
#define OCR_PIPELINE_BUFFER_POOL_H

/**
 * @file buffer_pool.h
 * @brief Fixed-capacity, reference-counted DMA-BUF pool.
 */

#include "buffer.h"
#include "thread.h"

#include <stdint.h>

#define OCR_POOL_MAX_BUFS 32

typedef enum {
    OCR_POOL_SLOT_UNUSED = 0,
    OCR_POOL_SLOT_FREE,
    OCR_POOL_SLOT_IN_USE,
} ocr_pool_slot_state_t;

/** Optional notification invoked when the final reference returns a slot. */
typedef int (*ocr_pool_recycle_fn)(ocr_buffer_t *buf, void *user_data);

typedef struct {
    ocr_mutex_t   lock;
    ocr_buffer_t  buffers[OCR_POOL_MAX_BUFS];
    int           count;             /* Slot capacity. */
    int           registered_count;  /* Slots owning a registered fd. */
    int           free_count;
    int           free_list[OCR_POOL_MAX_BUFS];
    uint8_t       slot_state[OCR_POOL_MAX_BUFS];
    ocr_pool_recycle_fn recycle_cb;
    void         *recycle_user_data;
    int           initialized;
} ocr_buffer_pool_t;

/** Initialize metadata. No DMA-BUF is allocated by this function. */
int ocr_pool_init(ocr_buffer_pool_t *pool, int count);

/**
 * Register a DMA-BUF and transfer ownership of fd to the pool.
 * A slot can be registered once between init and destroy.
 */
int ocr_pool_register(ocr_buffer_pool_t *pool, int index, int fd, size_t size,
                      uint32_t width, uint32_t height,
                      ocr_pixel_format_t format);

/**
 * Set a pool-wide recycle notification before any fd is registered.
 * The callback runs under the pool lock and must not call pool APIs.
 */
int ocr_pool_set_recycle_callback(ocr_buffer_pool_t *pool,
                                  ocr_pool_recycle_fn callback,
                                  void *user_data);

/** Return 1 when every registered slot is free, 0 when busy, or <0 on error. */
int ocr_pool_is_idle(ocr_buffer_pool_t *pool);

/** Acquire one reference to a registered free buffer, or NULL if exhausted. */
ocr_buffer_t *ocr_pool_acquire(ocr_buffer_pool_t *pool);

/** Acquire the exact registered slot (for APIs such as V4L2 DQBUF). */
ocr_buffer_t *ocr_pool_acquire_index(ocr_buffer_pool_t *pool, int index);

/**
 * Release one caller-owned reference. The buffer is recycled only when its
 * reference count reaches zero. Duplicate release is rejected.
 */
int ocr_pool_release(ocr_buffer_pool_t *pool, ocr_buffer_t *buf);

/**
 * Close registered fds and destroy the pool. Returns -2 while buffers are in
 * use, leaving the pool intact so callers can release them and retry.
 * Repeated calls after a successful destroy return 0.
 */
int ocr_pool_destroy(ocr_buffer_pool_t *pool);

#endif /* OCR_PIPELINE_BUFFER_POOL_H */
