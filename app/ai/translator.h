#ifndef OCR_AI_TRANSLATOR_H
#define OCR_AI_TRANSLATOR_H

/** @file translator.h @brief Encoder/decoder RKNN translation wrapper. */

#include "rknn_runtime.h"
#include "tokenizer.h"

#define TRANS_MAX_TOKENS 256

typedef struct {
    ocr_rknn_t      encoder;
    ocr_rknn_t      decoder;
    ocr_tokenizer_t src_tokenizer;
    ocr_tokenizer_t tgt_tokenizer;
    int             decoder_token_input;
    int             decoder_state_input;
    int             src_capacity;
    int             tgt_capacity;
    int             decoder_steps;
    int             max_len;
    int             initialized;
} ocr_translator_t;

int translator_init(ocr_translator_t *trs, const char *enc_model,
                    const char *dec_model, const char *src_vocab,
                    const char *tgt_vocab);

/** Validate the deployed tokenizer contract and fixed language pair. */
int translator_validate_manifest(const char *manifest_path,
                                 const char *src_lang,
                                 const char *tgt_lang);

int translator_translate(ocr_translator_t *trs, const char *src,
                         char *tgt, int tgt_size);

void translator_destroy(ocr_translator_t *trs);

#endif /* OCR_AI_TRANSLATOR_H */
