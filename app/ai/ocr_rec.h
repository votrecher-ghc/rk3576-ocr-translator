#ifndef OCR_AI_OCR_REC_H
#define OCR_AI_OCR_REC_H
/**
 * @file ocr_rec.h
 * @brief 文字识别：透视矫正→resize→RKNN推理→CTC解码→文本
 */

#include "rknn_runtime.h"
#include "ocr_postproc.h"
#include "buffer.h"

#define OCR_REC_INPUT_W 320
#define OCR_REC_INPUT_H 32
#define OCR_MAX_TEXT_LEN 256

/** 文字识别上下文 */
typedef struct {
    ocr_rknn_t rknn;           /* RKNN 运行时 */
    float     *pre_buf;        /* 预处理缓冲 */
    int        input_w;        /* 模型输入宽 */
    int        input_h;        /* 模型输入高 */
    int        vocab_size;     /* 词表大小 */
    char      *vocab;          /* 词表字符串数组 */
    int        vocab_len;      /* 词表条目数 */
} ocr_rec_t;

/**
 * @brief 初始化文字识别
 * @param[in] rec         识别上下文
 * @param[in] model_path  识别模型路径
 * @param[in] vocab_path  词表路径（每行一个字符）
 * @return 0=成功，负数=错误
 */
int ocr_rec_init(ocr_rec_t *rec, const char *model_path, const char *vocab_path);

/**
 * @brief 对单个文本框执行识别
 * @param[in]  rec   识别上下文
 * @param[in]  buf   原始图像
 * @param[in]  box   文本框（四点）
 * @param[out] text  识别结果（UTF-8）
 * @param[in]  text_size text 缓冲大小
 * @return 0=成功，负数=错误
 */
int ocr_rec_run(ocr_rec_t *rec, const ocr_buffer_t *buf,
                const ocr_text_box_t *box, char *text, int text_size);

/**
 * @brief 批量识别
 * @param[in]  rec    识别上下文
 * @param[in]  buf    原始图像
 * @param[in]  boxes  文本框列表
 * @param[out] texts  识别结果数组（每个元素为 char*）
 * @return 0=成功，负数=错误
 */
int ocr_rec_run_batch(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_list_t *boxes, char **texts);

/**
 * @brief 销毁文字识别
 */
void ocr_rec_destroy(ocr_rec_t *rec);

#endif /* OCR_AI_OCR_REC_H */
