/**
 * @file ocr_rec.c
 * @brief 文字识别实现
 */
#include "ocr_rec.h"
#include "rga_api.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* 加载词表（每行一个字符/token） */
static int load_vocab(const char *path, char **vocab, int *vocab_len)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_E("打开词表失败: %s", path);
        return -1;
    }

    char line[256];
    int count = 0;
    /* 先计数 */
    while (fgets(line, sizeof(line), fp)) count++;
    rewind(fp);

    *vocab = (char *)malloc((size_t)count * 64); /* 每条最多 64 字节 */
    if (!*vocab) { fclose(fp); return -2; }

    int idx = 0;
    while (fgets(line, sizeof(line), fp) && idx < count) {
        /* 去除换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        strncpy(*vocab + (size_t)idx * 64, line, 63);
        (*vocab + (size_t)idx * 64)[63] = '\0';
        idx++;
    }
    fclose(fp);
    *vocab_len = idx;
    return 0;
}

int ocr_rec_init(ocr_rec_t *rec, const char *model_path, const char *vocab_path)
{
    if (!rec || !model_path) return -1;
    memset(rec, 0, sizeof(*rec));

    if (ocr_rknn_load(&rec->rknn, model_path) != 0) return -2;
    rec->input_w = OCR_REC_INPUT_W;
    rec->input_h = OCR_REC_INPUT_H;

    if (vocab_path) {
        if (load_vocab(vocab_path, &rec->vocab, &rec->vocab_len) != 0) {
            LOG_W("词表加载失败，CTC 解码将不可用");
        }
        rec->vocab_size = rec->vocab_len;
    }

    rec->pre_buf = (float *)malloc((size_t)rec->input_w * rec->input_h * 3 * sizeof(float));
    if (!rec->pre_buf) { ocr_rec_destroy(rec); return -3; }
    return 0;
}

/* 透视矫正 + resize 到模型输入尺寸 */
static int warp_and_resize(ocr_rec_t *rec, const ocr_buffer_t *buf,
                           const ocr_text_box_t *box)
{
    /* TODO: 使用 OpenCV 或手动计算透视变换矩阵，
     * 通过 RGA 或 CPU 将文本框区域矫正并 resize 到 input_w x input_h */
    (void)rec; (void)buf; (void)box;
    return 0;
}

/* CTC 解码（贪婪） */
static int ctc_decode(const float *output, int timesteps, int vocab_size,
                      const char *vocab, char *text, int text_size)
{
    int text_idx = 0;
    int prev_idx = -1; /* 上一个非 blank 的索引 */

    for (int t = 0; t < timesteps && text_idx < text_size - 1; t++) {
        /* 找最大概率的字符索引 */
        int max_idx = 0;
        float max_val = output[t * vocab_size];
        for (int c = 1; c < vocab_size; c++) {
            float val = output[t * vocab_size + c];
            if (val > max_val) { max_val = val; max_idx = c; }
        }

        /* CTC blank 通常是索引 0 */
        if (max_idx != 0 && max_idx != prev_idx) {
            if (vocab && max_idx < vocab_size) {
                const char *ch = vocab + (size_t)max_idx * 64;
                size_t ch_len = strlen(ch);
                if (text_idx + ch_len < (size_t)text_size - 1) {
                    strcpy(text + text_idx, ch);
                    text_idx += (int)ch_len;
                }
            }
        }
        prev_idx = max_idx;
    }
    text[text_idx] = '\0';
    return text_idx;
}

int ocr_rec_run(ocr_rec_t *rec, const ocr_buffer_t *buf,
                const ocr_text_box_t *box, char *text, int text_size)
{
    if (!rec || !buf || !box || !text) return -1;

    /* 透视矫正 + resize */
    if (warp_and_resize(rec, buf, box) != 0) return -2;

    /* 预处理：归一化 */
    /* TODO: 填充 pre_buf */

    /* 设置输入 + 推理 */
    uint32_t input_size = (uint32_t)(rec->input_w * rec->input_h * 3 * sizeof(float));
    if (ocr_rknn_set_input(&rec->rknn, 0, rec->pre_buf, input_size) != 0) return -3;
    if (ocr_rknn_run(&rec->rknn) != 0) return -4;

    /* 获取输出 */
    void *out_data = NULL;
    uint32_t out_size = 0;
    if (ocr_rknn_get_output(&rec->rknn, 0, &out_data, &out_size) != 0 || !out_data) return -5;

    /* CTC 解码 */
    int timesteps = rec->input_w / 4; /* 通常下采样 */
    text[0] = '\0';
    ctc_decode((const float *)out_data, timesteps, rec->vocab_size,
               rec->vocab, text, text_size);
    return 0;
}

int ocr_rec_run_batch(ocr_rec_t *rec, const ocr_buffer_t *buf,
                      const ocr_text_box_list_t *boxes, char **texts)
{
    if (!rec || !buf || !boxes || !texts) return -1;
    for (int i = 0; i < boxes->count; i++) {
        if (!boxes->boxes[i].valid) continue;
        ocr_rec_run(rec, buf, &boxes->boxes[i], texts[i], OCR_MAX_TEXT_LEN);
    }
    return 0;
}

void ocr_rec_destroy(ocr_rec_t *rec)
{
    if (!rec) return;
    ocr_rknn_destroy(&rec->rknn);
    free(rec->pre_buf);
    free(rec->vocab);
    memset(rec, 0, sizeof(*rec));
}
