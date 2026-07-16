#ifndef OCR_AI_OCR_DET_H
#define OCR_AI_OCR_DET_H

/** @file ocr_det.h @brief OCR text detection. */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"

#define OCR_DET_INPUT_W 960
#define OCR_DET_INPUT_H 960

typedef struct {
    ocr_rknn_t rknn;
    float     *pre_buf;
    uint8_t   *resize_buf;
    uint8_t   *post_binary;
    size_t    *post_queue;
    size_t     post_capacity;
    int        input_w;
    int        input_h;
    int        output_w;
    int        output_h;
    float      thresh;
    float      box_thresh;
    float      unclip_ratio;
} ocr_det_t;

int ocr_det_init(ocr_det_t *det, const char *model_path);

int ocr_det_run(ocr_det_t *det, const ocr_buffer_t *buf,
                ocr_text_box_list_t *boxes);

void ocr_det_destroy(ocr_det_t *det);

#endif /* OCR_AI_OCR_DET_H */
