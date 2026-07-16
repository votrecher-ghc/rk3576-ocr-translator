/** @file translator.c @brief Contract-checked greedy RKNN translation. */
#include "translator.h"
#include "log.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cjson/cJSON.h>

static int manifest_string_equals(const cJSON *root, const char *key,
                                  const char *expected)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring && expected &&
           strcmp(item->valuestring, expected) == 0;
}

int translator_validate_manifest(const char *manifest_path,
                                 const char *src_lang,
                                 const char *tgt_lang)
{
    if (!manifest_path || !*manifest_path || !src_lang || !*src_lang ||
        !tgt_lang || !*tgt_lang) return -EINVAL;

    FILE *fp = fopen(manifest_path, "rb");
    if (!fp) return errno ? -errno : -ENOENT;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -EIO;
    }
    long length = ftell(fp);
    if (length <= 0 || length > 64 * 1024 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -EFBIG;
    }
    char *json = malloc((size_t)length + 1U);
    if (!json) {
        fclose(fp);
        return -ENOMEM;
    }
    size_t read_size = fread(json, 1, (size_t)length, fp);
    int close_ret = fclose(fp);
    if (read_size != (size_t)length || close_ret != 0) {
        free(json);
        return -EIO;
    }
    json[read_size] = '\0';

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return -EINVAL;
    const cJSON *max_vocab =
        cJSON_GetObjectItemCaseSensitive(root, "max_vocab");
    const cJSON *max_token_bytes =
        cJSON_GetObjectItemCaseSensitive(root, "max_token_bytes");
    int valid = manifest_string_equals(root, "contract",
                                       "static-encoder-decoder-v1") &&
                manifest_string_equals(root, "tokenizer",
                                       "greedy-vocab-v1") &&
                manifest_string_equals(root, "src_lang", src_lang) &&
                manifest_string_equals(root, "tgt_lang", tgt_lang) &&
                cJSON_IsNumber(max_vocab) &&
                max_vocab->valueint == TOKENIZER_MAX_VOCAB &&
                cJSON_IsNumber(max_token_bytes) &&
                max_token_bytes->valueint == TOKENIZER_MAX_TOKEN_LEN - 1;
    cJSON_Delete(root);
    if (!valid) {
        LOG_E("translation manifest does not match runtime contract/language pair: %s",
              manifest_path);
        return -EPROTO;
    }
    return 0;
}

static int is_token_type(uint32_t type)
{
    return type == RKNN_TENSOR_INT32 || type == RKNN_TENSOR_INT64;
}

static int is_convertible_state_type(uint32_t type)
{
    return type == RKNN_TENSOR_FLOAT32 || type == RKNN_TENSOR_FLOAT16 ||
           type == RKNN_TENSOR_INT8 || type == RKNN_TENSOR_UINT8 ||
           type == RKNN_TENSOR_INT16 || type == RKNN_TENSOR_UINT16 ||
           type == RKNN_TENSOR_INT32;
}

static int same_tensor_shape(const ocr_rknn_tensor_t *left,
                             const ocr_rknn_tensor_t *right)
{
    if (left->n_dims != right->n_dims || left->n_elems != right->n_elems ||
        left->fmt != right->fmt) {
        return 0;
    }
    for (uint32_t i = 0; i < left->n_dims; ++i) {
        if (left->dims[i] != right->dims[i]) return 0;
    }
    return 1;
}

static int validate_special_tokens(const ocr_tokenizer_t *tok,
                                   const char *label)
{
    if (tok->pad_id < 0 || tok->bos_id < 0 || tok->eos_id < 0 ||
        tok->unk_id < 0) {
        LOG_E("%s translation vocabulary must define PAD/BOS/EOS/UNK", label);
        return -EINVAL;
    }
    return 0;
}

static int validate_contract(ocr_translator_t *trs)
{
    if (trs->encoder.input_num != 1 || trs->encoder.output_num != 1) {
        LOG_E("encoder contract must be exactly one token input and one state output");
        return -EINVAL;
    }
    if (!is_token_type(trs->encoder.inputs[0].type) ||
        trs->encoder.inputs[0].n_dims != 2 ||
        trs->encoder.inputs[0].dims[0] != 1) {
        LOG_E("encoder input must be a static [1, sequence] INT32/INT64 tensor");
        return -EINVAL;
    }
    if (trs->decoder.input_num != 2 || trs->decoder.output_num != 1) {
        LOG_E("decoder contract must be two inputs (tokens,state) and one logits output");
        return -EINVAL;
    }

    trs->decoder_token_input = -1;
    trs->decoder_state_input = -1;
    for (int i = 0; i < trs->decoder.input_num; ++i) {
        if (is_token_type(trs->decoder.inputs[i].type) &&
            trs->decoder.inputs[i].n_dims == 2 &&
            trs->decoder.inputs[i].dims[0] == 1) {
            if (trs->decoder_token_input >= 0) {
                LOG_E("decoder has an ambiguous token-input contract");
                return -EINVAL;
            }
            trs->decoder_token_input = i;
        } else {
            trs->decoder_state_input = i;
        }
    }
    if (trs->decoder_token_input < 0 || trs->decoder_state_input < 0) {
        LOG_E("decoder token/state inputs cannot be identified from tensor attributes");
        return -EINVAL;
    }

    const ocr_rknn_tensor_t *state_input =
        &trs->decoder.inputs[trs->decoder_state_input];
    if (!is_convertible_state_type(state_input->type) ||
        !same_tensor_shape(&trs->encoder.outputs[0], state_input)) {
        LOG_E("encoder output and decoder state input shapes/layouts do not match");
        return -EINVAL;
    }

    const ocr_rknn_tensor_t *logits = &trs->decoder.outputs[0];
    if (logits->n_dims < 2 || logits->dims[0] != 1 ||
        logits->dims[logits->n_dims - 1u] !=
            (uint32_t)trs->tgt_tokenizer.vocab_size ||
        logits->n_elems % (uint32_t)trs->tgt_tokenizer.vocab_size != 0) {
        LOG_E("decoder logits must end in the target vocabulary dimension");
        return -EINVAL;
    }

    trs->src_capacity = (int)trs->encoder.inputs[0].n_elems;
    trs->tgt_capacity = (int)trs->decoder.inputs[trs->decoder_token_input].n_elems;
    trs->decoder_steps = (int)(logits->n_elems /
                               (uint32_t)trs->tgt_tokenizer.vocab_size);
    if (trs->src_capacity <= 0 || trs->src_capacity > TRANS_MAX_TOKENS ||
        trs->tgt_capacity <= 1 || trs->tgt_capacity > TRANS_MAX_TOKENS ||
        (trs->decoder_steps != 1 && trs->decoder_steps < trs->tgt_capacity)) {
        LOG_E("unsupported static translation sequence/logits dimensions");
        return -EINVAL;
    }
    trs->max_len = trs->tgt_capacity;
    return 0;
}

int translator_init(ocr_translator_t *trs, const char *enc_model,
                    const char *dec_model, const char *src_vocab,
                    const char *tgt_vocab)
{
    int ret;

    if (!trs || !enc_model || !dec_model || !src_vocab || !tgt_vocab) {
        return -EINVAL;
    }
    memset(trs, 0, sizeof(*trs));
    trs->decoder_token_input = trs->decoder_state_input = -1;

    ret = tokenizer_init(&trs->src_tokenizer, src_vocab, TOKENIZER_GREEDY);
    if (ret != 0) goto fail;
    ret = tokenizer_init(&trs->tgt_tokenizer, tgt_vocab, TOKENIZER_GREEDY);
    if (ret != 0) goto fail;
    ret = validate_special_tokens(&trs->src_tokenizer, "source");
    if (ret != 0) goto fail;
    ret = validate_special_tokens(&trs->tgt_tokenizer, "target");
    if (ret != 0) goto fail;

    ret = ocr_rknn_load(&trs->encoder, enc_model);
    if (ret != 0) goto fail;
    ret = ocr_rknn_load(&trs->decoder, dec_model);
    if (ret != 0) goto fail;
    ret = validate_contract(trs);
    if (ret != 0) goto fail;

    trs->src_tokenizer.max_len = trs->src_capacity;
    trs->tgt_tokenizer.max_len = trs->tgt_capacity;
    trs->initialized = 1;
    LOG_I("translator initialized: source=%d target=%d decoder_steps=%d",
          trs->src_capacity, trs->tgt_capacity, trs->decoder_steps);
    return 0;

fail:
    translator_destroy(trs);
    return ret;
}

static int set_token_input(ocr_rknn_t *model, int input_index,
                           const int32_t *ids, int capacity)
{
    if (!model || !ids || capacity <= 0 || capacity > TRANS_MAX_TOKENS) {
        return -EINVAL;
    }
    return ocr_rknn_set_input_ex(model, input_index, ids,
                                 (uint32_t)((size_t)capacity * sizeof(*ids)),
                                 RKNN_TENSOR_INT32, RKNN_TENSOR_UNDEFINED);
}

int translator_translate(ocr_translator_t *trs, const char *src,
                         char *tgt, int tgt_size)
{
    int32_t src_ids[TRANS_MAX_TOKENS];
    int32_t tgt_ids[TRANS_MAX_TOKENS];
    void *encoder_state = NULL;
    uint32_t encoder_state_size = 0;
    int ret;

    if (!trs || !trs->initialized || !src || !tgt || tgt_size <= 0) {
        return -EINVAL;
    }
    tgt[0] = '\0';

    int src_len = tokenizer_encode(&trs->src_tokenizer, src, src_ids,
                                   trs->src_capacity);
    if (src_len < 0) return src_len;
    for (int i = src_len; i < trs->src_capacity; ++i) {
        src_ids[i] = trs->src_tokenizer.pad_id;
    }
    ret = set_token_input(&trs->encoder, 0, src_ids, trs->src_capacity);
    if (ret != 0) return ret;
    ret = ocr_rknn_run(&trs->encoder);
    if (ret != 0) return ret;
    ret = ocr_rknn_get_output(&trs->encoder, 0, &encoder_state,
                              &encoder_state_size);
    if (ret != 0) return ret;
    uint64_t expected_state =
        (uint64_t)trs->encoder.outputs[0].n_elems * sizeof(float);
    if (!encoder_state || expected_state != encoder_state_size) {
        return -EMSGSIZE;
    }

    for (int i = 0; i < trs->tgt_capacity; ++i) {
        tgt_ids[i] = trs->tgt_tokenizer.pad_id;
    }
    tgt_ids[0] = trs->tgt_tokenizer.bos_id;
    int tgt_len = 1;

    while (tgt_len < trs->max_len) {
        ret = set_token_input(&trs->decoder, trs->decoder_token_input,
                              tgt_ids, trs->tgt_capacity);
        if (ret != 0) return ret;
        ret = ocr_rknn_set_input_ex(
            &trs->decoder, trs->decoder_state_input,
            encoder_state, encoder_state_size,
            RKNN_TENSOR_FLOAT32,
            (rknn_tensor_format)trs->encoder.outputs[0].fmt);
        if (ret != 0) return ret;
        ret = ocr_rknn_run(&trs->decoder);
        if (ret != 0) return ret;

        void *decoder_output = NULL;
        uint32_t decoder_output_size = 0;
        ret = ocr_rknn_get_output(&trs->decoder, 0, &decoder_output,
                                  &decoder_output_size);
        if (ret != 0) return ret;
        uint64_t expected_logits =
            (uint64_t)trs->decoder.outputs[0].n_elems * sizeof(float);
        if (!decoder_output || expected_logits != decoder_output_size) {
            return -EMSGSIZE;
        }

        int vocab_size = trs->tgt_tokenizer.vocab_size;
        int output_step = trs->decoder_steps == 1 ? 0 : tgt_len - 1;
        const float *logits = (const float *)decoder_output +
                              (size_t)output_step * (size_t)vocab_size;
        int best_id = -1;
        float best_value = -FLT_MAX;
        for (int token = 0; token < vocab_size; ++token) {
            if (isfinite(logits[token]) &&
                (best_id < 0 || logits[token] > best_value)) {
                best_value = logits[token];
                best_id = token;
            }
        }
        if (best_id < 0) return -EDOM;
        tgt_ids[tgt_len++] = best_id;
        if (best_id == trs->tgt_tokenizer.eos_id) break;
    }

    ret = tokenizer_decode(&trs->tgt_tokenizer, tgt_ids, tgt_len,
                           tgt, tgt_size);
    if (ret < 0) return ret;
    LOG_D("translation: '%s' -> '%s'", src, tgt);
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
    trs->decoder_token_input = trs->decoder_state_input = -1;
}
