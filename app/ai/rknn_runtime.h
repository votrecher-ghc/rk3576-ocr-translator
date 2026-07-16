#ifndef OCR_AI_RKNN_RUNTIME_H
#define OCR_AI_RKNN_RUNTIME_H

/**
 * @file rknn_runtime.h
 * @brief Small, ownership-safe wrapper around the RKNN runtime API.
 */

#include <stdint.h>

#include "rknn_api.h"

#define RKNN_MAX_IO 16
#define OCR_RKNN_MAX_DIMS 16

/** Tensor metadata copied from rknn_tensor_attr. */
typedef struct {
    uint32_t index;
    uint32_t dims[OCR_RKNN_MAX_DIMS];
    uint32_t n_dims;
    uint32_t n_elems;
    uint32_t type;
    uint32_t size;
    uint32_t fmt;
    uint32_t qnt_type;
    int8_t   fractional_length;
    int32_t  zero_point;
    float    scale;
    uint32_t width_stride;
    uint32_t height_stride;
    uint32_t size_with_stride;
    char     name[64];
} ocr_rknn_tensor_t;

/** RKNN model/runtime context. Output pointers are owned by RKNN. */
typedef struct {
    rknn_context      rknn_ctx;
    int               input_num;
    int               output_num;
    ocr_rknn_tensor_t inputs[RKNN_MAX_IO];
    ocr_rknn_tensor_t outputs[RKNN_MAX_IO];
    const void       *input_bufs[RKNN_MAX_IO];
    void             *output_bufs[RKNN_MAX_IO];
    uint32_t          output_sizes[RKNN_MAX_IO];
    int               outputs_acquired;
    int               initialized;
} ocr_rknn_t;

/** Load a model and query all input/output attributes. */
int ocr_rknn_load(ocr_rknn_t *rknn, const char *model_path);

/**
 * Set an input whose memory already uses the model tensor's type and format.
 * The caller retains ownership and must keep data alive through ocr_rknn_run().
 */
int ocr_rknn_set_input(ocr_rknn_t *rknn, int idx,
                       const void *data, uint32_t size);

/**
 * Set an input while explicitly describing the caller-side type and layout.
 * RKNN performs supported type/layout conversion because pass_through is zero.
 */
int ocr_rknn_set_input_ex(ocr_rknn_t *rknn, int idx,
                          const void *data, uint32_t size,
                          rknn_tensor_type type, rknn_tensor_format fmt);

/** DMA-BUF input is unsupported until an RKNN memory-object contract is chosen. */
int ocr_rknn_set_input_dmabuf(ocr_rknn_t *rknn, int idx,
                              int fd, uint32_t size);

/** Run inference and acquire float32 outputs owned by RKNN. */
int ocr_rknn_run(ocr_rknn_t *rknn);

/** Borrow an output pointer. It remains valid until the next run or destroy. */
int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx,
                        void **data, uint32_t *size);

/** Release outstanding outputs and destroy the RKNN context. */
void ocr_rknn_destroy(ocr_rknn_t *rknn);

#endif /* OCR_AI_RKNN_RUNTIME_H */
