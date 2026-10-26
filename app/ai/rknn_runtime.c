/**
 * @file rknn_runtime.c
 * @brief Ownership-safe RKNN runtime wrapper.
 *
 * RKNN 运行时封装：加载模型和张量属性，设置普通内存输入，运行推理并管理输出生命周期。
 * 输出指针由上下文持有，下次推理或销毁前有效；DMA-BUF 导入因缺少原生张量布局约定而返回不支持。
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

/** @brief 复制模型张量形状、类型和量化属性，后续输入校验以这些属性为准。 */
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

/** @brief 归还 RKNN 持有的上次输出，并清空本地输出指针，防止重复释放。 */
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

/** @brief 读取模型、创建上下文并查询输入输出属性；任何步骤失败都撤销已分配资源。 */
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

    /* flags=0 走普通上下文初始化；model 是临时文件内容，初始化结束立即释放。
     * 后续运行依赖 rknn_ctx，不再依赖这份加载文件用的堆缓冲。 */
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

    /* 查询每个索引的真实属性，不能只知道输入输出数量就假定布局。
     * 保存的类型、元素数和格式既供预处理选择路径，也用于 set_input 的长度校验。 */
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

/** @brief 校验输入元素大小与数量后提交普通内存张量，允许运行时按模型要求转换。 */
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
    /* 校验调用者提供的源类型所需字节数，而非模型内部量化存储的字节数。
     * 例如模型内部 INT8、源张量 FLOAT32，源缓冲仍应为 n_elems*4 字节。 */
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
    /* pass_through=0 允许运行时根据源 type/fmt 转换到模型输入要求。
     * 这里既没有传 DMA-BUF fd，也没有设置原生零拷贝 IO memory。 */
    input.pass_through = 0;
    input.type = type;
    input.fmt = fmt;

    int ret = rknn_inputs_set(rknn->rknn_ctx, 1, &input);
    if (ret < 0) {
        LOG_E("rknn_inputs_set(%d) failed: %d", idx, ret);
        return -EIO;
    }
    /* 只记录借用指针，用于 run 前检查本轮每个输入都已设置；不接管 data，
     * 上层须保证输入缓冲在同步推理期间有效，尤其不能让另一个线程覆盖工作区。 */
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

/** @brief 占位接口：尚未定义模型原生布局及 stride 约定，明确返回 -ENOTSUP。 */
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

/** @brief 释放上次输出、执行推理并获取本次输出；当前输出统一请求浮点格式。 */
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

    /* 开始新一轮前先归还上次结果，所有通过 get_output 借出的旧指针随即失效。
     * encoder 和 decoder 各有独立上下文，所以 decoder 运行不会释放 encoder 状态。 */
    release_outputs(rknn);

    int ret = rknn_run(rknn->rknn_ctx, NULL);
    /* 清空本轮输入标记，强制下一轮重新设置全部输入，避免误用旧张量或悬空指针。 */
    memset(rknn->input_bufs, 0, sizeof(rknn->input_bufs));
    if (ret < 0) {
        LOG_E("rknn_run failed: %d", ret);
        return -EIO;
    }

    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < rknn->output_num; ++i) {
        outputs[i].index = (uint32_t)i;
        /* 统一浮点输出，OCR/翻译后处理无需各自处理 INT8 反量化。
         * is_prealloc=0 由 RKNN 分配返回缓冲，只能 outputs_release，不能 free。 */
        outputs[i].want_float = 1;
        outputs[i].is_prealloc = 0;
    }

    ret = rknn_outputs_get(rknn->rknn_ctx, (uint32_t)rknn->output_num,
                           outputs, NULL);
    if (ret < 0) {
        LOG_E("rknn_outputs_get failed: %d", ret);
        /* 有的运行时可能在报错前已返回部分输出；检查实际指针后释放，
         * 不能因为 outputs_get 最终失败就假定没有资源需要归还。 */
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

/** @brief 借用上下文持有的输出指针；调用者不能 free，下一次推理前应完成使用。 */
int ocr_rknn_get_output(ocr_rknn_t *rknn, int idx,
                        void **data, uint32_t *size)
{
    if (!rknn || !rknn->initialized || !rknn->outputs_acquired ||
        idx < 0 || idx >= rknn->output_num || !data) {
        return -EINVAL;
    }
    /* 这里只返回视图而不复制；需要跨下一轮推理长期保存时，上层应另行复制。
     * 输出不是 ocr_buffer_t，不能用其 ref/unref 管理。 */
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
