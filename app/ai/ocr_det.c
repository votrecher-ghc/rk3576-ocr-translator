/**
 * @file ocr_det.c
 * @brief 文字检测实现
 */
#include "ocr_det.h"
#include "rga_api.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

int ocr_det_init(ocr_det_t *det, const char *model_path)
{
    if (!det || !model_path) return -1;
    memset(det, 0, sizeof(*det));

    if (ocr_rknn_load(&det->rknn, model_path) != 0) {
        return -2;
    }

    det->input_w = OCR_DET_INPUT_W;
    det->input_h = OCR_DET_INPUT_H;
    det->thresh = 0.3f;
    det->box_thresh = 0.6f;
    det->unclip_ratio = 1.6f;

    /* 预分配缓冲 */
    det->resize_buf = (uint8_t *)malloc((size_t)det->input_w * det->input_h * 3);
    det->pre_buf = (float *)malloc((size_t)det->input_w * det->input_h * 3 * sizeof(float));
    if (!det->resize_buf || !det->pre_buf) {
        ocr_det_destroy(det);
        return -3;
    }
    return 0;
}

/* 预处理：resize + 归一化 + 减均值除方差 */
static int preprocess(ocr_det_t *det, const ocr_buffer_t *buf)
{
    /* TODO: 使用 RGA 将输入图像 resize 到 input_w x input_h，格式转为 RGB888 */
    /* 当前简化：直接拷贝（假设输入已是正确尺寸） */

    /* 归一化：均值 [0.485, 0.456, 0.406]，方差 [0.229, 0.224, 0.225] */
    const float mean[3] = {0.485f, 0.456f, 0.406f};
    const float std[3]  = {0.229f, 0.224f, 0.225f};
    int size = det->input_w * det->input_h;
    for (int i = 0; i < size; i++) {
        for (int c = 0; c < 3; c++) {
            float val = det->resize_buf[i * 3 + c] / 255.0f;
            det->pre_buf[c * size + i] = (val - mean[c]) / std[c];
        }
    }
    return 0;
}

int ocr_det_run(ocr_det_t *det, const ocr_buffer_t *buf, ocr_text_box_list_t *boxes)
{
    if (!det || !buf || !boxes) return -1;
    memset(boxes, 0, sizeof(*boxes));

    /* 预处理 */
    if (preprocess(det, buf) != 0) return -2;

    /* 设置输入 */
    uint32_t input_size = (uint32_t)(det->input_w * det->input_h * 3 * sizeof(float));
    if (ocr_rknn_set_input(&det->rknn, 0, det->pre_buf, input_size) != 0) return -3;

    /* 推理 */
    if (ocr_rknn_run(&det->rknn) != 0) return -4;

    /* 获取输出（概率图） */
    void *out_data = NULL;
    uint32_t out_size = 0;
    if (ocr_rknn_get_output(&det->rknn, 0, &out_data, &out_size) != 0 || !out_data) return -5;

    /* 后处理（DB） */
    int out_w = det->input_w / 4;  /* 通常输出下采样 4 倍 */
    int out_h = det->input_h / 4;
    /* TODO: 根据 out_size 推断实际输出尺寸 */
    float *prob_map = (float *)out_data;
    if (ocr_db_postprocess(prob_map, out_w, out_h,
                           det->thresh, det->box_thresh, det->unclip_ratio,
                           boxes) != 0) {
        return -6;
    }

    /* 将检测框坐标从模型输入尺寸映射回原图 */
    float sx = (float)buf->width / det->input_w;
    float sy = (float)buf->height / det->input_h;
    for (int i = 0; i < boxes->count; i++) {
        for (int j = 0; j < 4; j++) {
            boxes->boxes[i].x[j] *= sx;
            boxes->boxes[i].y[j] *= sy;
        }
    }

    ocr_sort_boxes(boxes);
    return 0;
}

void ocr_det_destroy(ocr_det_t *det)
{
    if (!det) return;
    ocr_rknn_destroy(&det->rknn);
    free(det->resize_buf);
    free(det->pre_buf);
    memset(det, 0, sizeof(*det));
}
