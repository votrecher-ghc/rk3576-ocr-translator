/** @file ocr_rec.c @brief OCR recognition and UTF-8 CTC decoding. */
#include "ocr_rec.h"
#include "image_preproc.h"
#include "log.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int utf8_valid(const unsigned char *text, size_t length)
{
    size_t pos = 0;
    while (pos < length) {
        unsigned char first = text[pos];
        int chars;
        uint32_t cp;
        if (first < 0x80) {
            ++pos;
            continue;
        } else if (first >= 0xC2 && first <= 0xDF) {
            chars = 2; cp = first & 0x1Fu;
        } else if (first >= 0xE0 && first <= 0xEF) {
            chars = 3; cp = first & 0x0Fu;
        } else if (first >= 0xF0 && first <= 0xF4) {
            chars = 4; cp = first & 0x07u;
        } else {
            return 0;
        }
        if (length - pos < (size_t)chars) return 0;
        for (int i = 1; i < chars; ++i) {
            if ((text[pos + (size_t)i] & 0xC0u) != 0x80u) return 0;
            cp = (cp << 6) | (text[pos + (size_t)i] & 0x3Fu);
        }
        if ((chars == 3 && cp < 0x800u) || (chars == 4 && cp < 0x10000u) ||
            (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
            return 0;
        }
        pos += (size_t)chars;
    }
    return 1;
}

static int load_vocab(const char *path,
                      char (**vocab)[OCR_REC_MAX_TOKEN_LEN], int *vocab_len)
{
    char line[OCR_REC_MAX_TOKEN_LEN + 2];
    char (*entries)[OCR_REC_MAX_TOKEN_LEN] = NULL;
    size_t capacity = 0;
    int count = 0;
    int ret = 0;
    FILE *fp;

    if (!path || !vocab || !vocab_len) return -EINVAL;
    *vocab = NULL;
    *vocab_len = 0;
    fp = fopen(path, "rb");
    if (!fp) return errno ? -errno : -ENOENT;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        int has_newline = len > 0 && line[len - 1] == '\n';
        if (!has_newline && !feof(fp)) {
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) {}
            ret = -E2BIG;
            break;
        }
        if (has_newline) line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        if (count == 0 && len >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF) {
            memmove(line, line + 3, len - 2);
            len -= 3;
        }
        if (len >= OCR_REC_MAX_TOKEN_LEN ||
            !utf8_valid((const unsigned char *)line, len)) {
            ret = -EILSEQ;
            break;
        }
        if ((size_t)count == capacity) {
            size_t new_capacity = capacity == 0 ? 1024 : capacity * 2;
            void *new_entries;
            if (new_capacity > 100000u) {
                ret = -E2BIG;
                break;
            }
            new_entries = realloc(entries, new_capacity * sizeof(*entries));
            if (!new_entries) {
                ret = -ENOMEM;
                break;
            }
            entries = new_entries;
            capacity = new_capacity;
        }
        memset(entries[count], 0, sizeof(entries[count]));
        memcpy(entries[count], line, len + 1);
        ++count;
    }
    if (ferror(fp) && ret == 0) ret = -EIO;
    if (fclose(fp) != 0 && ret == 0) ret = -EIO;
    if (ret != 0 || count == 0) {
        free(entries);
        return ret != 0 ? ret : -ENODATA;
    }

    *vocab = entries;
    *vocab_len = count;
    return 0;
}

static int is_blank_token(const char *token)
{
    return token[0] == '\0' || strcmp(token, "<blank>") == 0 ||
           strcmp(token, "[blank]") == 0 || strcmp(token, "[BLANK]") == 0 ||
           strcmp(token, "<ctc_blank>") == 0;
}

static int image_tensor_shape(const ocr_rknn_tensor_t *attr,
                              int *width, int *height)
{
    if (!attr || !width || !height || attr->n_dims != 4 || attr->dims[0] != 1) {
        return -EINVAL;
    }
    if (attr->fmt == RKNN_TENSOR_NCHW) {
        if (attr->dims[1] != 3) return -EINVAL;
        *height = (int)attr->dims[2];
        *width = (int)attr->dims[3];
    } else if (attr->fmt == RKNN_TENSOR_NHWC) {
        if (attr->dims[3] != 3) return -EINVAL;
        *height = (int)attr->dims[1];
        *width = (int)attr->dims[2];
    } else {
        return -ENOTSUP;
    }
    return *width > 0 && *height > 0 &&
           (uint64_t)*width * (uint64_t)*height * 3u == attr->n_elems
         ? 0 : -EINVAL;
}

static int configure_ctc_contract(ocr_rec_t *rec)
{
    const ocr_rknn_tensor_t *output = &rec->rknn.outputs[0];
    if (output->n_dims < 2 || output->dims[0] != 1) return -EINVAL;

    uint32_t classes = output->dims[output->n_dims - 1u];
    if (classes == 0 || output->n_elems % classes != 0) return -EINVAL;
    rec->vocab_size = (int)classes;
    rec->timesteps = (int)(output->n_elems / classes);

    int explicit_blank = -1;
    int explicit_space = -1;
    for (int i = 0; i < rec->vocab_len; ++i) {
        if (is_blank_token(rec->vocab[i])) explicit_blank = i;
        if (strcmp(rec->vocab[i], " ") == 0) explicit_space = i;
    }

    /* PaddleOCR commonly keeps blank and the optional space outside the file. */
    if (classes == (uint32_t)rec->vocab_len + 2u &&
        explicit_blank < 0 && explicit_space < 0) {
        void *new_vocab = realloc(
            rec->vocab, (size_t)(rec->vocab_len + 1) * sizeof(*rec->vocab));
        if (!new_vocab) return -ENOMEM;
        rec->vocab = new_vocab;
        memset(rec->vocab[rec->vocab_len], 0,
               sizeof(rec->vocab[rec->vocab_len]));
        rec->vocab[rec->vocab_len][0] = ' ';
        ++rec->vocab_len;
    }

    if (classes == (uint32_t)rec->vocab_len + 1u && explicit_blank < 0) {
        rec->blank_id = 0;
        rec->class_to_vocab_offset = -1;
        return 0;
    }
    if (classes == (uint32_t)rec->vocab_len) {
        if (explicit_blank >= 0) {
            rec->blank_id = explicit_blank;
            rec->class_to_vocab_offset = 0;
            return 0;
        }
    }
    return -EINVAL;
}

int ocr_rec_init(ocr_rec_t *rec, const char *model_path,
                 const char *vocab_path)
{
    size_t pixels;
    int ret;

    if (!rec || !model_path || !vocab_path) return -EINVAL;
    memset(rec, 0, sizeof(*rec));

    ret = load_vocab(vocab_path, &rec->vocab, &rec->vocab_len);
    if (ret != 0) {
        LOG_E("failed to load OCR dictionary: %d", ret);
        goto fail;
    }
    ret = ocr_rknn_load(&rec->rknn, model_path);
    if (ret != 0) goto fail;
    if (rec->rknn.input_num != 1 || rec->rknn.output_num != 1) {
        LOG_E("recognizer requires exactly one image input and one CTC output");
        ret = -EINVAL;
        goto fail;
    }
    ret = image_tensor_shape(&rec->rknn.inputs[0], &rec->input_w, &rec->input_h);
    if (ret != 0) {
        LOG_E("unsupported recognizer input tensor contract");
        goto fail;
    }
    ret = configure_ctc_contract(rec);
    if (ret != 0) {
        LOG_E("CTC class count does not match the UTF-8 dictionary/blank contract");
        goto fail;
    }

    rknn_tensor_type input_type = (rknn_tensor_type)rec->rknn.inputs[0].type;
    if (input_type != RKNN_TENSOR_UINT8 && input_type != RKNN_TENSOR_INT8 &&
        input_type != RKNN_TENSOR_FLOAT16 && input_type != RKNN_TENSOR_FLOAT32) {
        ret = -ENOTSUP;
        goto fail;
    }

    pixels = (size_t)rec->input_w * (size_t)rec->input_h;
    if (pixels > SIZE_MAX / (3u * sizeof(float))) {
        ret = -EOVERFLOW;
        goto fail;
    }
    rec->resize_buf = malloc(pixels * 3u);
    rec->pre_buf = malloc(pixels * 3u * sizeof(float));
    if (!rec->resize_buf || !rec->pre_buf) {
        ret = -ENOMEM;
        goto fail;
    }
    return 0;

fail:
    ocr_rec_destroy(rec);
    return ret;
}

static int preprocess(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_t *box, const void **input,
                      uint32_t *input_size, rknn_tensor_type *input_type,
                      rknn_tensor_format *input_fmt)
{
    size_t pixels = (size_t)rec->input_w * (size_t)rec->input_h;
    int ret = ocr_ai_warp_quad_rgb(buf, box, rec->resize_buf,
                                   rec->input_w, rec->input_h);
    if (ret != 0) {
        LOG_E("recognizer preprocessing requires a mapped, tightly packed supported image: %d", ret);
        return ret;
    }

    rknn_tensor_type model_type = (rknn_tensor_type)rec->rknn.inputs[0].type;
    if (model_type == RKNN_TENSOR_UINT8 || model_type == RKNN_TENSOR_INT8) {
        *input = rec->resize_buf;
        *input_size = (uint32_t)(pixels * 3u);
        *input_type = RKNN_TENSOR_UINT8;
        *input_fmt = RKNN_TENSOR_NHWC;
        return 0;
    }

    for (size_t i = 0; i < pixels; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            float value = rec->resize_buf[i * 3u + (size_t)channel] / 255.0f;
            rec->pre_buf[(size_t)channel * pixels + i] = (value - 0.5f) / 0.5f;
        }
    }
    *input = rec->pre_buf;
    *input_size = (uint32_t)(pixels * 3u * sizeof(float));
    *input_type = RKNN_TENSOR_FLOAT32;
    *input_fmt = RKNN_TENSOR_NCHW;
    return 0;
}

static int ctc_decode(const float *output, int timesteps, int classes,
                      int blank_id, int class_to_vocab_offset,
                      char (*vocab)[OCR_REC_MAX_TOKEN_LEN], int vocab_len,
                      char *text, int text_size)
{
    int pos = 0;
    int previous = -1;
    text[0] = '\0';

    for (int step = 0; step < timesteps; ++step) {
        const float *row = output + (size_t)step * (size_t)classes;
        int best = -1;
        float best_value = -FLT_MAX;
        for (int cls = 0; cls < classes; ++cls) {
            if (isfinite(row[cls]) && (best < 0 || row[cls] > best_value)) {
                best_value = row[cls];
                best = cls;
            }
        }
        if (best < 0) return -EDOM;

        if (best != blank_id && best != previous) {
            int vocab_id = best + class_to_vocab_offset;
            if (vocab_id < 0 || vocab_id >= vocab_len) return -ERANGE;
            size_t token_len = strlen(vocab[vocab_id]);
            if (token_len > (size_t)(text_size - 1 - pos)) return -ENOSPC;
            memcpy(text + pos, vocab[vocab_id], token_len);
            pos += (int)token_len;
            text[pos] = '\0';
        }
        previous = best;
    }
    return pos;
}

int ocr_rec_run(ocr_rec_t *rec, const ocr_buffer_t *buf,
                const ocr_text_box_t *box, char *text, int text_size)
{
    const void *input;
    uint32_t input_size;
    rknn_tensor_type input_type;
    rknn_tensor_format input_fmt;
    void *output = NULL;
    uint32_t output_size = 0;
    int ret;

    if (!rec || !rec->rknn.initialized || !buf || !box ||
        !text || text_size <= 0) {
        return -EINVAL;
    }
    text[0] = '\0';
    ret = preprocess(rec, buf, box, &input, &input_size,
                     &input_type, &input_fmt);
    if (ret != 0) return ret;
    ret = ocr_rknn_set_input_ex(&rec->rknn, 0, input, input_size,
                                input_type, input_fmt);
    if (ret != 0) return ret;
    ret = ocr_rknn_run(&rec->rknn);
    if (ret != 0) return ret;
    ret = ocr_rknn_get_output(&rec->rknn, 0, &output, &output_size);
    if (ret != 0) return ret;

    uint64_t expected = (uint64_t)rec->rknn.outputs[0].n_elems * sizeof(float);
    if (!output || expected != output_size) return -EMSGSIZE;
    ret = ctc_decode((const float *)output, rec->timesteps, rec->vocab_size,
                     rec->blank_id, rec->class_to_vocab_offset,
                     rec->vocab, rec->vocab_len, text, text_size);
    return ret < 0 ? ret : 0;
}

int ocr_rec_run_batch(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_list_t *boxes, char **texts)
{
    if (!rec || !buf || !boxes || !texts || boxes->count < 0 ||
        boxes->count > OCR_MAX_TEXT_BOXES) {
        return -EINVAL;
    }
    for (int i = 0; i < boxes->count; ++i) {
        if (!texts[i]) return -EINVAL;
        texts[i][0] = '\0';
        if (!boxes->boxes[i].valid) continue;
        int ret = ocr_rec_run(rec, buf, &boxes->boxes[i], texts[i],
                              OCR_MAX_TEXT_LEN);
        if (ret != 0) return ret;
    }
    return 0;
}

void ocr_rec_destroy(ocr_rec_t *rec)
{
    if (!rec) return;
    ocr_rknn_destroy(&rec->rknn);
    free(rec->pre_buf);
    free(rec->resize_buf);
    free(rec->vocab);
    memset(rec, 0, sizeof(*rec));
}
