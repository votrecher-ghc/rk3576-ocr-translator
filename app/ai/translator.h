#ifndef OCR_AI_TRANSLATOR_H
#define OCR_AI_TRANSLATOR_H

/** @file translator.h @brief Encoder/decoder RKNN translation wrapper.
 *
 * 离线翻译：校验 manifest 和静态 encoder/decoder 张量约定，再执行逐 token 贪心解码。
 * encoder 先生成状态；decoder 反复读取目标前缀和状态，直到遇到 EOS 或达到长度上限。
 */

#include "rknn_runtime.h"
#include "tokenizer.h"

#define TRANS_MAX_TOKENS 256

typedef struct {
    ocr_rknn_t      encoder; /**< 本翻译器拥有的源文本模型上下文；每句运行一次，输出状态保留到本句结束。 */
    ocr_rknn_t      decoder; /**< 本翻译器拥有的目标生成模型上下文；逐 token 运行，与 encoder 的输出生命周期独立。 */
    ocr_tokenizer_t src_tokenizer; /**< 拥有源语言词表的分词器，用于把原文转成 encoder token ID。 */
    ocr_tokenizer_t tgt_tokenizer; /**< 拥有目标语言词表的分词器，用于 BOS/EOS 选择及将生成 ID 还原成文字。 */
    int             decoder_token_input; /**< 由类型/形状推断出的 decoder token 输入索引；不能假定总是输入 0。 */
    int             decoder_state_input; /**< decoder 的 encoder 状态输入索引，与 decoder_token_input 区分。 */
    int             src_capacity; /**< encoder 静态源序列容量，token 数；不足处补源 PAD。 */
    int             tgt_capacity; /**< decoder 静态目标前缀容量，token 数；包括 BOS 和未生成位置的 PAD。 */
    int             decoder_steps; /**< decoder 输出 logits 中的时间步数；1 表示仅下一步，否则从整段中取 tgt_len-1。 */
    int             max_len; /**< 本句最大生成序列长度，含 BOS；初始化取 tgt_capacity，也受循环终止条件约束。 */
    int             initialized; /**< 两个模型、词表及张量/特殊 token 约定均通过检查时为 1。 */
} ocr_translator_t;

/**
 * @brief 加载两份模型和两份词表，校验静态序列及状态张量契约
 * @param[out] trs 待初始化的翻译器；已有实例须先销毁
 * @param[in] enc_model 源语言 encoder RKNN 模型路径
 * @param[in] dec_model 目标语言 decoder RKNN 模型路径
 * @param[in] src_vocab 源词表路径，须含 PAD/BOS/EOS/UNK
 * @param[in] tgt_vocab 目标词表路径，须含 PAD/BOS/EOS/UNK
 * @return 0=成功，负数=加载、契约校验或资源分配失败
 * @note 本函数不读取 manifest；部署层应另外调用 translator_validate_manifest。
 */
int translator_init(ocr_translator_t *trs, const char *enc_model,
                    const char *dec_model, const char *src_vocab,
                    const char *tgt_vocab);

/**
 * @brief 检查部署清单声明的契约版本、词表上限和固定语言对
 * @param[in] manifest_path JSON 清单文件路径
 * @param[in] src_lang 期望的源语言代码，须与清单完全一致
 * @param[in] tgt_lang 期望的目标语言代码，须与清单完全一致
 * @return 0=匹配，-EPROTO=声明不匹配，其他负数=参数、文件或 JSON 错误
 * @note 这里只校验清单声明；实际模型张量及特殊 token 由 translator_init 校验。
 */
int translator_validate_manifest(const char *manifest_path,
                                 const char *src_lang,
                                 const char *tgt_lang);

/**
 * @brief 同步翻译一句 UTF-8 原文，使用逐 token 贪心解码
 * @param[in,out] trs 已初始化的翻译器，同一实例须串行调用
 * @param[in] src 以 NUL 结尾的 UTF-8 原文；超出源序列容量时由分词器截断
 * @param[out] tgt 调用者分配的译文缓冲
 * @param[in] tgt_size 译文容量，以字节计，包含结尾 NUL
 * @return 0=成功，负数=编码、推理或解码失败（含 -ENOSPC 输出不足）
 * @note 遇到 EOS 或达到 max_len 即结束；达到长度上限也返回成功。
 *       失败时译文可能是部分结果；返回值不是输出长度。
 */
int translator_translate(ocr_translator_t *trs, const char *src,
                         char *tgt, int tgt_size);

/** @brief 所有翻译调用结束后释放模型及词表；不释放调用者的原文和译文缓冲。 */
void translator_destroy(ocr_translator_t *trs);

#endif /* OCR_AI_TRANSLATOR_H */
