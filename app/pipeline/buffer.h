#ifndef OCR_PIPELINE_BUFFER_H
#define OCR_PIPELINE_BUFFER_H

/** @file buffer.h @brief DMA-BUF metadata and reference counting. */

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum {
    OCR_FMT_UNKNOWN = 0,
    OCR_FMT_NV12,
    OCR_FMT_NV16,
    OCR_FMT_YUYV,
    OCR_FMT_RGB565,
    OCR_FMT_RGB888,
    OCR_FMT_ARGB8888,
    OCR_FMT_BGRA8888,
} ocr_pixel_format_t;

struct ocr_buffer;
typedef void (*ocr_buffer_release_fn)(struct ocr_buffer *buf,
                                      void *user_data);

typedef struct ocr_buffer {
    int               fd;        /* DMA-BUF descriptor; -1 when absent. */
    void             *mmap_addr;
    size_t            size;
    uint32_t          plane_count; /* Logical image planes sharing fd. */
    uint32_t          strides[4];  /* Bytes per row for each plane. */
    uint32_t          offsets[4];  /* Byte offsets within fd. */
    uint32_t          width;
    uint32_t          height;
    ocr_pixel_format_t format;
    uint32_t          index;
    atomic_int        refcount;
    uint64_t          timestamp;
    uint64_t          frame_id;
    int               owned;
    ocr_buffer_release_fn release_cb;
    void             *release_user_data;
} ocr_buffer_t;

/** Initialize standalone metadata with one caller-owned reference. */
int ocr_buffer_init(ocr_buffer_t *buf);

/**
 * Configure the zero-reference callback before publishing the buffer.
 * The caller must have exclusive access while changing it.
 */
int ocr_buffer_set_release_callback(ocr_buffer_t *buf,
                                    ocr_buffer_release_fn release_cb,
                                    void *user_data);

/** Configure logical plane layout for DRM/RGA import. */
int ocr_buffer_set_layout(ocr_buffer_t *buf, uint32_t plane_count,
                          const uint32_t *strides, const uint32_t *offsets);

/** Add a reference; negative means invalid, overflow, or already released. */
int ocr_buffer_ref(ocr_buffer_t *buf);

/**
 * Release a reference and invoke release_cb exactly once on transition to 0.
 * Duplicate release is rejected, so the count never underflows.
 */
int ocr_buffer_unref(ocr_buffer_t *buf);

int ocr_buffer_mmap(ocr_buffer_t *buf);
void ocr_buffer_munmap(ocr_buffer_t *buf);
const char *ocr_pixel_format_str(ocr_pixel_format_t fmt);

#endif /* OCR_PIPELINE_BUFFER_H */
