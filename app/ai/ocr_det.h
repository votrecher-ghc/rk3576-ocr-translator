#ifndef OCR_AI_OCR_DET_H
#define OCR_AI_OCR_DET_H
/**
 * @file ocr_det.h
 * @brief 文字检测：输入预处理→RKNN推理→DB后处理→文本框列表
 */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"
#include "node.h"

#define OCR_DET_INPUT_W 320
#define OCR_DET_INPUT_H 320

/** 文字检测上下文 */
typedef struct {
    ocr_rknn_t    rknn;          /* RKNN 运行时 */
    float        *pre_buf;       /* 预处理缓冲（归一化后的 float） */
    uint8_t      *resize_buf;    /* resize 后的 uint8 缓冲 */
    int           input_w;       /* 模型输入宽 */
    int           input_h;       /* 模型输入高 */
    float         thresh;        /* 二值化阈值 */
    float         box_thresh;    /* 框过滤阈值 */
    float         unclip_ratio;  /* 膨胀系数 */
} ocr_det_t;

/**
 * @brief 初始化文字检测
 * @param[in] det        检测上下文
 * @param[in] model_path 检测模型路径
 * @return 0=成功，负数=错误
 */
int ocr_det_init(ocr_det_t *det, const char *model_path);

/**
 * @brief 执行文字检测
 * @param[in]  det   检测上下文
 * @param[in]  buf   输入图像缓冲
 * @param[out] boxes 输出文本框列表
 * @return 0=成功，负数=错误
 */
int ocr_det_run(ocr_det_t *det, const ocr_buffer_t *buf, ocr_text_box_list_t *boxes);

/**
 * @brief 销毁文字检测
 */
void ocr_det_destroy(ocr_det_t *det);

#endif /* OCR_AI_OCR_DET_H */
