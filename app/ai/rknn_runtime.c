/**
 * @file rknn_runtime.c
 * @brief Ownership-safe RKNN runtime wrapper.
 */
#include "rknn_runtime.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int tensor_type_size(rknn_tensor_type type)
{
    switch (type) {
    case RKNN_TENSOR_FLOAT32:
    case RKNN_TENSOR_INT32:
    case RKNN_TENSOR_UINT32:
        return 4;
    case RKNN_TENSOR_FLOAT16:
    case RKNN_TENSOR_INT16:
    case RKNN_TENSOR_UINT16:
        return 2;
    case RKNN_TENSOR_INT8:
    case RKNN_TENSOR_UINT8:
        return 1;
    case RKNN_TENSOR_INT64:
        return 8;
    case RKNN_TENSOR_BOOL:
        return 1;
    default:
        return 0;
    }
}

static void copy_tensor_attr(ocr_rknn_tensor_t *dst,
                             const rknn_tensor_attr *src)
{
    uint32_t n_dims = src->n_dims;
    if (n_dims > OCR_RKNN_MAX_DIMS) n_dims = OCR_RKNN_MAX_DIMS;

    memset(dst, 0, sizeof(*dst));
    dst->index = src->index;
    dst->n_dims = n_dims;
    dst->n_elems = src->n_elems;
    dst->type = (uint32_t)src->type;
    dst->size = src->size;
    dst->fmt = (uint32_t)src->fmt;
    dst->qnt_type = (uint32_t)src->qnt_type;
    dst->fractional_length = src->fl;
    dst->zero_point = src->zp;
    dst->scale = src->scale;
    dst->width_stride = src->w_stride;
    dst->height_stride = src->h_stride;
    dst->size_with_stride = src->size_with_stride;
    if (n_dims > 0) {
        memcpy(dst->dims, src->dims, (size_t)n_dims * sizeof(dst->dims[0]));
    }
    size_t name_len = 0;
    while (name_len < sizeof(src->name) && src->name[name_len] != '\0') {
        ++name_len;
    }
    if (name_len >= sizeof(dst->name)) name_len = sizeof(dst->name) - 1u;
    memcpy(dst->name, src->name, name_len);
    dst->name[name_len] = '\0';
}

static void release_outputs(ocr_rknn_t *rknn)
{
    rknn_output outputs[RKNN_MAX_IO];

    if (!rknn || !rknn->initialized || !rknn->outputs_acquired) return;

    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < rknn->output_num; ++i) {
        outputs[i].index = (uint32_t)i;
        outputs[i].want_float = 1;
        outputs[i].is_prealloc = 0;
        outputs[i].buf = rknn->output_bufs[i];
        outputs[i].size = rknn->output_sizes[i];
    }

    int ret = rknn_outputs_release(rknn->rknn_ctx,
                                   (uint32_t)rknn->output_num, outputs);
    if (ret < 0) {
        LOG_W("rknn_outputs_release failed: %d", ret);
    }
    memset(rknn->output_bufs, 0, sizeof(rknn->output_bufs));
    memset(rknn->output_sizes, 0, sizeof(rknn->output_sizes));
    rknn->outputs_acquired = 0;
}

int ocr_rknn_load(ocr_rknn_t *rknn, const char *model_path)
{
    struct stat st;
    FILE *fp = NULL;
    void *model = NULL;
    int context_created = 0;
    int ret;

    if (!rknn || !model_path || model_path[0] == '\0') return -EINVAL;
    memset(rknn, 0, sizeof(*rknn));

    if (stat(model_path, &st) != 0) {
        LOG_E("invalid RKNN model file: %s", model_path);
        return errno ? -errno : -ENOENT;
    }
    if (!S_ISREG(st.st_mode)) {
        LOG_E("RKNN model path is not a regular file: %s", model_path);
        return -EINVAL;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) {
        LOG_E("unsupported RKNN model size: %lld", (long long)st.st_size);
        return -EFBIG;
    }

    fp = fopen(model_path, "rb");
    if (!fp) {
        LOG_E("failed to open RKNN model %s: %s", model_path, strerror(errno));
        return -errno;
    }

    model = malloc((size_t)st.st_size);
    if (!model) {
        fclose(fp);
        return -ENOMEM;
    }
    if (fread(model, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) {
        int saved_errno = ferror(fp) && errno != 0 ? errno : EIO;
        free(model);
        fclose(fp);
        return -saved_errno;
    }
    if (fclose(fp) != 0) {
        free(model);
        return -EIO;
    }
    fp = NULL;

    ret = rknn_init(&rknn->rknn_ctx, model, (uint32_t)st.st_size, 0, NULL);
    free(model);
    model = NULL;
    if (ret < 0) {
        LOG_E("rknn_init failed: %d", ret);
        memset(rknn, 0, sizeof(*rknn));
        return -EIO;
    }
    context_created = 1;

    rknn_input_output_num io_num;
    memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(rknn->rknn_ctx, RKNN_QUERY_IN_OUT_NUM,
                     &io_num, sizeof(io_num));
    if (ret < 0 || io_num.n_input == 0 || io_num.n_output == 0 ||
        io_num.n_input > RKNN_MAX_IO || io_num.n_output > RKNN_MAX_IO) {
        LOG_E("invalid RKNN I/O contract (ret=%d in=%u out=%u)", ret,
              io_num.n_input, io_num.n_output);
        goto fail;
    }
    rknn->input_num = (int)io_num.n_input;
    rknn->output_num = (int)io_num.n_output;

    for (int i = 0; i < rknn->input_num; ++i) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = (uint32_t)i;
        ret = rknn_query(rknn->rknn_ctx, RKNN_QUERY_INPUT_ATTR,
                         &attr, sizeof(attr));
        if (ret < 0 || attr.n_dims > OCR_RKNN_MAX_DIMS ||
            attr.n_elems == 0 || attr.size == 0) {
            LOG_E("invalid RKNN input attribute %d (ret=%d)", i, ret);
            goto fail;
        }
        copy_tensor_attr(&rknn->inputs[i], &attr);
    }

    for (int i = 0; i < rknn->output_num; ++i) {
        rknn_tensor_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.index = (uint32_t)i;
        ret = rknn_query(rknn->rknn_ctx, RKNN_QUERY_OUTPUT_ATTR,
                         &attr, sizeof(attr));
        if (ret < 0 || attr.n_dims > OCR_RKNN_MAX_DIMS ||
            attr.n_elems == 0 || attr.size == 0) {
            LOG_E("invalid RKNN output attribute %d (ret=%d)", i, ret);
            goto fail;
        }
        copy_tensor_attr(&rknn->outputs[i], &attr);
    }

    rknn->initialized = 1;
    LOG_I("RKNN model loaded: %s (in=%d out=%d)", model_path,
          rknn->input_num, rknn->output_num);
    return 0;

fail:
    if (context_created) rknn_destroy(rknn->rknn_ctx);
    memset(rknn, 0, sizeof(*rknn));
    return -EINVAL;
}

int ocr_rknn_set_input_ex(ocr_rknn_t *rknn, int idx,
                          const void *data, uint32_t size,
                          rknn_tensor_type type, rknn_tensor_format fmt)
{
    rknn_input input;
    int element_size;
    uint64_t expected_size;

    if (!rknn || !rknn->initialized || idx < 0 || idx >= rknn->input_num ||
        !data || size == 0) {
        return -EINVAL;
    }

    element_size = tensor_type_size(type);
    if (element_size <= 0) return -ENOTSUP;
    expected_size = (uint64_t)rknn->inputs[idx].n_elems * (uint32_t)element_size;
    if (expected_size != size) {
        LOG_E("RKNN input %d size mismatch: got=%u expected=%llu", idx, size,
              (unsigned long long)expected_size);
        return -EMSGSIZE;
    }

    memset(&input, 0, sizeof(input));
    input.index = (uint32_t)idx;
    input.buf = (void *)data;
    input.size = size;
    input.pass_through = 0;
    input.type = type;
    input.fmt = fmt;

    int ret = rknn_inputs_set(rknn->rknn_ctx, 1, &input);
    if (ret < 0) {
        LOG_E("rknn_inputs_set(%d) failed: %d", idx, ret);
        return -EIO;
    }
    rknn->input_bufs[idx] = data;
    return 0;
}

int ocr_rknn_set_input(ocr_rknn_t *rknn, int idx,
                       const void *data, uint32_t size)
{
    if (!rknn || idx < 0 || idx >= rknn->input_num) return -EINVAL;
    return ocr_rknn_set_input_ex(
        rknn, idx, data, size,
        (rknn_tensor_type)rknn->inputs[idx].type,
        (rknn_tensor_format)rknn->inputs[idx].fmt);
}

int ocr_rknn_set_input_dmabuf(ocr_rknn_t *rknn, int idx,
                              int fd, uint32_t size)
{
    if (!rknn || !rknn->initialized || idx < 0 || idx >= rknn->input_num ||
        fd < 0 || size == 0) {
        return -EINVAL;
    }

    /* rknn_create_mem_from_fd/rknn_set_io_mem requires a model-specific native
     * tensor layout and stride contract. Silently copying or claiming zero-copy
     * here would be incorrect. */
    LOG_E("RKNN DMA-BUF input requires an explicit native tensor/stride contract");
    return -ENOTSUP;
}

int ocr_rknn_run(ocr_rknn_t *rknn)
{
    rknn_output outputs[RKNN_MAX_IO];

    if (!rknn || !rknn->initialized) return -EINVAL;
    for (int i = 0; i < rknn->input_num; ++i) {
        if (!rknn->input_bufs[i]) {
            LOG_E("RKNN input %d was not set", i);
            return -EINVAL;
        }
    }

    release_outputs(rknn);

    int ret = rknn_run(rknn->rknn_ctx, NULL);
    memset(rknn->input_bufs, 0, sizeof(rknn->input_bufs));
    if (ret < 0) {
        LOG_E("rknn_run failed: %d", ret);
        return -EIO;
    }

    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < rknn->output_num; ++i) {
        outputs[i].index = (uint32_t)i;
        outputs[i].want_float = 1;
        outputs[i].is_prealloc = 0;
    }

    ret = rknn_outputs_get(rknn->rknn_ctx, (uint32_t)rknn->output_num,
                           outputs, NULL);
    if (ret < 0) {
        LOG_E("rknn_outputs_get failed: %d", ret);
        /* Some runtime versions may have filled a subset before failing. */
        int has_output = 0;
        for (int i = 0; i < rknn->output_num; ++i) {
            if (outputs[i].buf) has_output = 1;
        }
        if (has_output) {
            (void)rknn_outputs_release(rknn->rknn_ctx,
                                       (uint32_t)rknn->output_num, outputs);
        }
        memset(rknn->output_bufs, 0, sizeof(rknn->output_bufs));
        memset(rknn->output_sizes, 0, sizeof(rknn->output_sizes));
        return -EIO;
    }

    for (int i = 0; i < rknn->output_num; ++i) {
        if (!outputs[i].buf || outputs[i].size == 0) {
            (void)rknn_outputs_release(rknn->rknn_ctx,
                                       (uint32_t)rknn->output_num, outputs);
            memset(rknn->output_bufs, 0, sizeof(rknn->output_bufs));
            memset(rknn->output_sizes, 0, sizeof(rknn->output_sizes));
            return -EIO;
        }
        rknn->output_bufs[i] = outputs[i].buf;
        rknn->output_sizes[i] = outputs[i].size;
    }
    rknn->outputs_acquired = 1;
    return 0;
}

int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx,
                        void **data, uint32_t *size)
{
    if (!rknn || !rknn->initialized || !rknn->outputs_acquired ||
        idx < 0 || idx >= rknn->output_num || !data) {
        return -EINVAL;
    }
    *data = rknn->output_bufs[idx];
    if (size) *size = rknn->output_sizes[idx];
    return *data ? 0 : -EIO;
}

void ocr_rknn_destroy(ocr_rknn_t *rknn)
{
    if (!rknn) return;
    if (rknn->initialized) {
        release_outputs(rknn);
        int ret = rknn_destroy(rknn->rknn_ctx);
        if (ret < 0) LOG_W("rknn_destroy failed: %d", ret);
    }
    memset(rknn, 0, sizeof(*rknn));
}
