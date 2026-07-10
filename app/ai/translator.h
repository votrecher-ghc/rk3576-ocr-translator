#ifndef OCR_AI_TRANSLATOR_H
#define OCR_AI_TRANSLATOR_H
/**
 * @file translator.h
 * @brief 翻译：tokenize→encode→自回归推理→解码
 *
 * 基于轻量 NMT 模型（RKNN）执行自回归翻译推理。
 */

#include "rknn_runtime.h"
#include "tokenizer.h"

#define TRANS_MAX_TOKENS 256

/** 翻译上下文 */
typedef struct {
    ocr_rknn_t       encoder;     /* 编码器 RKNN */
    ocr_rknn_t       decoder;     /* 解码器 RKNN */
    ocr_tokenizer_t  src_tokenizer;/* 源语言分词器 */
    ocr_tokenizer_t  tgt_tokenizer;/* 目标语言分词器 */
    int              max_len;     /* 最大生成长度 */
} ocr_translator_t;

/**
 * @brief 初始化翻译
 * @param[in] trs          翻译上下文
 * @param[in] enc_model    编码器模型路径
 * @param[in] dec_model    解码器模型路径
 * @param[in] src_vocab    源语言词表
 * @param[in] tgt_vocab    目标语言词表
 * @return 0=成功，负数=错误
 */
int translator_init(ocr_translator_t *trs, const char *enc_model,
                    const char *dec_model, const char *src_vocab,
                    const char *tgt_vocab);

/**
 * @brief 执行翻译
 * @param[in]  trs    翻译上下文
 * @param[in]  src    源文本（UTF-8）
 * @param[out] tgt    目标文本缓冲
 * @param[in]  tgt_size 目标缓冲大小
 * @return 0=成功，负数=错误
 */
int translator_translate(ocr_translator_t *trs, const char *src,
                         char *tgt, int tgt_size);

/**
 * @brief 销毁翻译
 */
void translator_destroy(ocr_translator_t *trs);

#endif /* OCR_AI_TRANSLATOR_H */
