/**
 * @file translator.c
 * @brief 翻译实现
 */
#include "translator.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

int translator_init(ocr_translator_t *trs, const char *enc_model,
                    const char *dec_model, const char *src_vocab,
                    const char *tgt_vocab)
{
    if (!trs || !enc_model || !dec_model) return -1;
    memset(trs, 0, sizeof(*trs));
    trs->max_len = TRANS_MAX_TOKENS;

    if (ocr_rknn_load(&trs->encoder, enc_model) != 0) return -2;
    if (ocr_rknn_load(&trs->decoder, dec_model) != 0) return -3;

    if (src_vocab && tokenizer_init(&trs->src_tokenizer, src_vocab, TOKENIZER_BPE) != 0) {
        LOG_W("源语言分词器加载失败");
    }
    if (tgt_vocab && tokenizer_init(&trs->tgt_tokenizer, tgt_vocab, TOKENIZER_BPE) != 0) {
        LOG_W("目标语言分词器加载失败");
    }

    LOG_I("翻译器初始化完成");
    return 0;
}

int translator_translate(ocr_translator_t *trs, const char *src,
                         char *tgt, int tgt_size)
{
    if (!trs || !src || !tgt) return -1;
    tgt[0] = '\0';

    /* 1. 源文本分词 */
    int32_t src_ids[TRANS_MAX_TOKENS];
    int src_len = tokenizer_encode(&trs->src_tokenizer, src, src_ids, TRANS_MAX_TOKENS);
    if (src_len <= 0) return -2;

    /* 2. 编码器推理 → 生成编码器隐状态 */
    uint32_t enc_input_size = (uint32_t)(src_len * sizeof(int32_t));
    if (ocr_rknn_set_input(&trs->encoder, 0, src_ids, enc_input_size) != 0) return -3;
    if (ocr_rknn_run(&trs->encoder) != 0) return -4;

    void *enc_out = NULL;
    uint32_t enc_out_size = 0;
    if (ocr_rknn_get_output(&trs->encoder, 0, &enc_out, &enc_out_size) != 0) return -5;

    /* 3. 自回归解码 */
    int32_t tgt_ids[TRANS_MAX_TOKENS];
    memset(tgt_ids, 0, sizeof(tgt_ids));
    tgt_ids[0] = trs->tgt_tokenizer.bos_id;
    int tgt_len = 1;

    for (int step = 0; step < trs->max_len - 1; step++) {
        /* 解码器输入：已生成的 token + 编码器输出 */
        uint32_t dec_input_size = (uint32_t)(tgt_len * sizeof(int32_t));
        ocr_rknn_set_input(&trs->decoder, 0, tgt_ids, dec_input_size);
        /* TODO: 将编码器隐状态作为 decoder 第二个输入 */
        ocr_rknn_set_input(&trs->decoder, 1, enc_out, enc_out_size);

        if (ocr_rknn_run(&trs->decoder) != 0) return -6;

        void *dec_out = NULL;
        uint32_t dec_out_size = 0;
        if (ocr_rknn_get_output(&trs->decoder, 0, &dec_out, &dec_out_size) != 0) return -7;

        /* 贪婪解码：取最后一步 logits 的 argmax */
        /* TODO: 根据 dec_out_size 推断 vocab_size 和步数 */
        const float *logits = (const float *)dec_out;
        int vocab_size = trs->tgt_tokenizer.vocab_size;
        int offset = (tgt_len - 1) * vocab_size;
        int best_id = 0;
        float best_val = logits[offset];
        for (int c = 1; c < vocab_size; c++) {
            if (logits[offset + c] > best_val) {
                best_val = logits[offset + c];
                best_id = c;
            }
        }

        tgt_ids[tgt_len++] = best_id;
        if (best_id == trs->tgt_tokenizer.eos_id) break;
    }

    /* 4. 解码为目标文本 */
    tokenizer_decode(&trs->tgt_tokenizer, tgt_ids, tgt_len, tgt, tgt_size);
    LOG_D("翻译: '%s' → '%s'", src, tgt);
    return 0;
}

void translator_destroy(ocr_translator_t *trs)
{
    if (!trs) return;
    ocr_rknn_destroy(&trs->encoder);
    ocr_rknn_destroy(&trs->decoder);
    tokenizer_destroy(&trs->src_tokenizer);
    tokenizer_destroy(&trs->tgt_tokenizer);
    memset(trs, 0, sizeof(*trs));
}
