#ifndef OCR_AI_OCR_POSTPROC_H
#define OCR_AI_OCR_POSTPROC_H
/**
 * @file ocr_postproc.h
 * @brief DB 后处理：二值化→findContours→四点框→排序
 *
 * 对文字检测模型输出的概率图执行 DB（Differentiable Binarization）后处理，
 * 得到文本框列表。
 */

#include <stdint.h>

#define OCR_MAX_TEXT_BOXES 256
#define OCR_BOX_POINTS 4

/** 文本框（四点） */
typedef struct {
    float x[OCR_BOX_POINTS]; /* 四个角的 x 坐标 */
    float y[OCR_BOX_POINTS]; /* 四个角的 y 坐标 */
    float score;             /* 置信度 */
    int   valid;             /* 是否有效 */
} ocr_text_box_t;

/** 文本框列表 */
typedef struct {
    ocr_text_box_t boxes[OCR_MAX_TEXT_BOXES];
    int count;
} ocr_text_box_list_t;

/**
 * @brief DB 后处理
 * @param[in]  prob_map    概率图（float，HxW，值域[0,1]）
 * @param[in]  width       概率图宽
 * @param[in]  height      概率图高
 * @param[in]  thresh      二值化阈值（如 0.3）
 * @param[in]  box_thresh  框过滤阈值（如 0.6）
 * @param[in]  unclip_ratio 膨胀系数（如 1.5）
 * @param[out] out         输出文本框列表
 * @return 0=成功，负数=错误
 */
int ocr_db_postprocess(const float *prob_map, int width, int height,
                       float thresh, float box_thresh, float unclip_ratio,
                       ocr_text_box_list_t *out);

/**
 * @brief 对文本框按 y 再按 x 排序（阅读顺序）
 */
int ocr_sort_boxes(ocr_text_box_list_t *list);

#endif /* OCR_AI_OCR_POSTPROC_H */
