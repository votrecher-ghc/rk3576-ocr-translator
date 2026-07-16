#ifndef OCR_AI_OCR_REC_H
#define OCR_AI_OCR_REC_H

/** @file ocr_rec.h @brief OCR recognition and UTF-8 CTC decoding. */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"

#define OCR_REC_INPUT_W 320
#define OCR_REC_INPUT_H 48
#define OCR_MAX_TEXT_LEN 256
#define OCR_REC_MAX_TOKEN_LEN 256

typedef struct {
    ocr_rknn_t rknn;
    float     *pre_buf;
    uint8_t   *resize_buf;
    int        input_w;
    int        input_h;
    int        vocab_size; /* number of model output classes */
    char     (*vocab)[OCR_REC_MAX_TOKEN_LEN];
    int        vocab_len;  /* number of entries in the dictionary file */
    int        blank_id;
    int        class_to_vocab_offset;
    int        timesteps;
} ocr_rec_t;

int ocr_rec_init(ocr_rec_t *rec, const char *model_path,
                 const char *vocab_path);

int ocr_rec_run(ocr_rec_t *rec, const ocr_buffer_t *buf,
                const ocr_text_box_t *box, char *text, int text_size);

int ocr_rec_run_batch(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_list_t *boxes, char **texts);

void ocr_rec_destroy(ocr_rec_t *rec);

#endif /* OCR_AI_OCR_REC_H */
