#ifndef OCR_AI_OCR_POSTPROC_H
#define OCR_AI_OCR_POSTPROC_H

/** @file ocr_postproc.h @brief DB probability-map post-processing. */

#include <stddef.h>
#include <stdint.h>

#define OCR_MAX_TEXT_BOXES 256
#define OCR_BOX_POINTS 4

typedef struct {
    float x[OCR_BOX_POINTS];
    float y[OCR_BOX_POINTS];
    float score;
    int   valid;
} ocr_text_box_t;

typedef struct {
    ocr_text_box_t boxes[OCR_MAX_TEXT_BOXES];
    int count;
} ocr_text_box_list_t;

/**
 * Threshold a float32 HxW map, extract 8-connected components, score and
 * fit and expand oriented four-point boxes, then sort in reading order.
 */
int ocr_db_postprocess(const float *prob_map, int width, int height,
                       float thresh, float box_thresh, float unclip_ratio,
                       ocr_text_box_list_t *out);

/** Allocation-free variant for per-frame use. Workspace needs width*height items. */
int ocr_db_postprocess_with_workspace(
    const float *prob_map, int width, int height,
    float thresh, float box_thresh, float unclip_ratio,
    uint8_t *binary, size_t *queue, size_t workspace_pixels,
    ocr_text_box_list_t *out);

int ocr_sort_boxes(ocr_text_box_list_t *list);

#endif /* OCR_AI_OCR_POSTPROC_H */
