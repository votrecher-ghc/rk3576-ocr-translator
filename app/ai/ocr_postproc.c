/**
 * @file ocr_postproc.c
 * @brief DB 后处理实现
 *
 * 简化版 DB 后处理，使用阈值二值化 + 连通域检测 + 最小外接矩形。
 * 完整实现可参考 PaddleOCR 的 DB postprocess。
 */
#include "ocr_postproc.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>

/* 简单的二值化 */
static void threshold(const float *src, uint8_t *dst, int w, int h, float thresh)
{
    for (int i = 0; i < w * h; i++) {
        dst[i] = src[i] > thresh ? 1 : 0;
    }
}

/* 简化的连通域标记 + 外接矩形（替代 findContours） */
static int find_boxes(const uint8_t *bin, int w, int h,
                      ocr_text_box_list_t *out, float box_thresh,
                      const float *prob_map)
{
    /* TODO: 实现完整的连通域检测算法
     * 当前使用简化的水平/垂直投影法仅做占位 */
    out->count = 0;

    /* 临时简化：将整个图作为一个框（仅用于流程验证） */
    int total = 0;
    for (int i = 0; i < w * h; i++) total += bin[i];
    if (total > 0 && out->count < OCR_MAX_TEXT_BOXES) {
        ocr_text_box_t *box = &out->boxes[out->count];
        box->x[0] = 0;        box->y[0] = 0;
        box->x[1] = (float)w; box->y[1] = 0;
        box->x[2] = (float)w; box->y[2] = (float)h;
        box->x[3] = 0;        box->y[3] = (float)h;
        box->score = (float)total / (w * h);
        box->valid = box->score > box_thresh ? 1 : 0;
        if (box->valid) out->count++;
    }

    return 0;
}

int ocr_db_postprocess(const float *prob_map, int width, int height,
                       float thresh, float box_thresh, float unclip_ratio,
                       ocr_text_box_list_t *out)
{
    if (!prob_map || !out || width <= 0 || height <= 0) return -1;
    memset(out, 0, sizeof(*out));

    uint8_t *bin = (uint8_t *)malloc((size_t)width * height);
    if (!bin) return -2;

    threshold(prob_map, bin, width, height, thresh);
    find_boxes(bin, width, height, out, box_thresh, prob_map);

    /* TODO: 对每个框执行 unclip（膨胀）扩展边界 */
    (void)unclip_ratio;

    free(bin);
    return 0;
}

/* 比较函数：先按 y 中心排序，再按 x 中心排序 */
static int box_compare(const void *a, const void *b)
{
    const ocr_text_box_t *ba = (const ocr_text_box_t *)a;
    const ocr_text_box_t *bb = (const ocr_text_box_t *)b;
    float ya = (ba->y[0] + ba->y[1] + ba->y[2] + ba->y[3]) / 4.0f;
    float yb = (bb->y[0] + bb->y[1] + bb->y[2] + bb->y[3]) / 4.0f;
    /* y 差距大于行高阈值视为不同行 */
    if (fabsf(ya - yb) > 10.0f) return (ya < yb) ? -1 : 1;
    float xa = (ba->x[0] + ba->x[1] + ba->x[2] + ba->x[3]) / 4.0f;
    float xb = (bb->x[0] + bb->x[1] + bb->x[2] + bb->x[3]) / 4.0f;
    return (xa < xb) ? -1 : (xa > xb) ? 1 : 0;
}

int ocr_sort_boxes(ocr_text_box_list_t *list)
{
    if (!list || list->count <= 0) return 0;
    qsort(list->boxes, list->count, sizeof(ocr_text_box_t), box_compare);
    return 0;
}
