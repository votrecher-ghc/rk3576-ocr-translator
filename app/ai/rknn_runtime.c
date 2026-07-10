/**
 * @file rknn_runtime.c
 * @brief RKNN 运行时封装实现
 */
#include "rknn_runtime.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "rknn_api.h"

int ocr_rknn_load(ocr_rknn_t *rknn, const char *model_path)
{
    if (!rknn || !model_path) return -1;
    memset(rknn, 0, sizeof(*rknn));

    /* 读取模型文件 */
    FILE *fp = fopen(model_path, "rb");
    if (!fp) {
        LOG_E("打开模型文件失败: %s", model_path);
        return -2;
    }
    fseek(fp, 0, SEEK_END);
    long model_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    void *model = malloc(model_len);
    if (!model) {
        fclose(fp);
        return -3;
    }
    if (fread(model, 1, model_len, fp) != (size_t)model_len) {
        free(model);
        fclose(fp);
        return -4;
    }
    fclose(fp);

    /* 初始化 RKNN 上下文 */
    rknn_context *ctx = (rknn_context *)malloc(sizeof(rknn_context));
    if (!ctx) {
        free(model);
        return -5;
    }

    int ret = rknn_init(ctx, model, model_len, 0, NULL);
    free(model); /* rknn_init 内部会拷贝模型 */
    if (ret < 0) {
        LOG_E("rknn_init 失败: %d", ret);
        free(ctx);
        return -6;
    }
    rknn->rknn_ctx = ctx;

    /* 查询输入输出属性 */
    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        LOG_W("rknn_query IO_NUM 失败: %d", ret);
    } else {
        rknn->input_num = io_num.input_num;
        rknn->output_num = io_num.output_num;
    }

    /* 查询输入属性 */
    for (uint32_t i = 0; i < (uint32_t)rknn->input_num && i < RKNN_MAX_IO; i++) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr, sizeof(attr)) == 0) {
            rknn->inputs[i].n_dims = attr.n_dims;
            memcpy(rknn->inputs[i].dims, attr.dims, sizeof(attr.dims));
            rknn->inputs[i].type = attr.type;
            rknn->inputs[i].size = attr.size;
            rknn->inputs[i].fmt = attr.fmt;
            strncpy(rknn->inputs[i].name, (char *)attr.name, sizeof(rknn->inputs[i].name) - 1);
        }
    }

    /* 查询输出属性 */
    for (uint32_t i = 0; i < (uint32_t)rknn->output_num && i < RKNN_MAX_IO; i++) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = i;
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr, sizeof(attr)) == 0) {
            rknn->outputs[i].n_dims = attr.n_dims;
            memcpy(rknn->outputs[i].dims, attr.dims, sizeof(attr.dims));
            rknn->outputs[i].type = attr.type;
            rknn->outputs[i].size = attr.size;
            rknn->outputs[i].fmt = attr.fmt;
            strncpy(rknn->outputs[i].name, (char *)attr.name, sizeof(rknn->outputs[i].name) - 1);
            /* 预分配输出缓冲 */
            rknn->output_bufs[i] = malloc(attr.size);
        }
    }

    rknn->initialized = 1;
    LOG_I("RKNN 模型加载成功: %s (in=%d out=%d)", model_path,
          rknn->input_num, rknn->output_num);
    return 0;
}

int ocr_rknn_set_input(ocr_rknn_t *rknn, int idx, const void *data, uint32_t size)
{
    if (!rknn || !rknn->rknn_ctx || idx < 0 || idx >= rknn->input_num) return -1;

    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index = idx;
    input.buf = (void *)data;
    input.size = size;
    input.pass_through = 0; /* 让 RKNN 做格式转换 */
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;

    int ret = rknn_inputs_set((rknn_context *)rknn->rknn_ctx, 1, &input);
    if (ret < 0) {
        LOG_E("rknn_inputs_set 失败: %d", ret);
        return -2;
    }
    rknn->input_bufs[idx] = (void *)data;
    return 0;
}

int ocr_rknn_set_input_dmabuf(ocr_rknn_t *rknn, int idx, int fd, uint32_t size)
{
    if (!rknn || !rknn->rknn_ctx || idx < 0) return -1;
    /* TODO: 使用 rknn_set_input_fd（如支持）实现 DMA-BUF 零拷贝输入 */
    /* 当前回退：mmap fd 后拷贝到输入 */
    (void)fd; (void)size;
    LOG_W("DMA-BUF 零拷贝输入待实现，回退到普通输入");
    return -2;
}

int ocr_rknn_run(ocr_rknn_t *rknn)
{
    if (!rknn || !rknn->rknn_ctx) return -1;
    int ret = rknn_run((rknn_context *)rknn->rknn_ctx, NULL);
    if (ret < 0) {
        LOG_E("rknn_run 失败: %d", ret);
        return -2;
    }

    /* 获取输出 */
    rknn_output outputs[RKNN_MAX_IO];
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < rknn->output_num && i < RKNN_MAX_IO; i++) {
        outputs[i].index = i;
        outputs[i].want_float = 0;
        outputs[i].buf = rknn->output_bufs[i];
        outputs[i].size = rknn->outputs[i].size;
    }
    ret = rknn_outputs_get((rknn_context *)rknn->rknn_ctx, rknn->output_num, outputs, NULL);
    if (ret < 0) {
        LOG_E("rknn_outputs_get 失败: %d", ret);
        return -3;
    }
    return 0;
}

int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx, void **data, uint32_t *size)
{
    if (!rknn || idx < 0 || idx >= rknn->output_num) return -1;
    if (data) *data = rknn->output_bufs[idx];
    if (size) *size = rknn->outputs[idx].size;
    return 0;
}

void ocr_rknn_destroy(ocr_rknn_t *rknn)
{
    if (!rknn || !rknn->rknn_ctx) return;
    for (int i = 0; i < rknn->output_num; i++) {
        free(rknn->output_bufs[i]);
        rknn->output_bufs[i] = NULL;
    }
    rknn_destroy((rknn_context *)rknn->rknn_ctx);
    free(rknn->rknn_ctx);
    rknn->rknn_ctx = NULL;
    rknn->initialized = 0;
}
