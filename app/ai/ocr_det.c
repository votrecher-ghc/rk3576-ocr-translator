/** @file ocr_det.c @brief OCR detection preprocessing and DB decoding.
 *
 * 文字检测流程：CPU 缩放图像并准备张量 -> RKNN 推理 -> 概率图连通域后处理。
 * 模型加载时检查输入/输出形状，预分配工作区；同一个上下文不应被多个推理线程并发使用。
 */
#include "ocr_det.h"
#include "image_preproc.h"
#include "log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/** @brief 从模型属性识别 NCHW/NHWC 的高宽，并拒绝不支持的批次或通道布局。 */
static int image_tensor_shape(const ocr_rknn_tensor_t *attr,
                              int expected_channels, int *width, int *height)
{
    if (!attr || !width || !height || attr->n_dims != 4 || attr->dims[0] != 1) {
        return -EINVAL;
    }
    if (attr->fmt == RKNN_TENSOR_NCHW) {
        if (attr->dims[1] != (uint32_t)expected_channels) return -EINVAL;
        *height = (int)attr->dims[2];
        *width = (int)attr->dims[3];
    } else if (attr->fmt == RKNN_TENSOR_NHWC) {
        if (attr->dims[3] != (uint32_t)expected_channels) return -EINVAL;
        *height = (int)attr->dims[1];
        *width = (int)attr->dims[2];
    } else {
        return -ENOTSUP;
    }
    if (*width <= 0 || *height <= 0 ||
        (uint64_t)*width * (uint64_t)*height * (uint32_t)expected_channels !=
        attr->n_elems) {
        return -EINVAL;
    }
    return 0;
}

/** @brief 确认检测输出可以解释为单张概率图，拒绝不明确的模型输出。 */
static int output_map_shape(const ocr_rknn_tensor_t *attr,
                            int *width, int *height)
{
    if (!attr || !width || !height || attr->n_dims != 4 || attr->dims[0] != 1) {
        return -EINVAL;
    }
    if (attr->fmt == RKNN_TENSOR_NCHW) {
        if (attr->dims[1] != 1) return -EINVAL;
        *height = (int)attr->dims[2];
        *width = (int)attr->dims[3];
    } else if (attr->fmt == RKNN_TENSOR_NHWC) {
        if (attr->dims[3] != 1) return -EINVAL;
        *height = (int)attr->dims[1];
        *width = (int)attr->dims[2];
    } else {
        return -ENOTSUP;
    }
    return *width > 0 && *height > 0 &&
           (uint64_t)*width * (uint64_t)*height == attr->n_elems
         ? 0 : -EINVAL;
}

/** @brief 加载模型并提前分配图像预处理和连通域工作区，减少逐帧堆内存分配。 */
int ocr_det_init(ocr_det_t *det, const char *model_path)
{
    size_t pixels;
    size_t post_pixels;
    int ret;

    if (!det || !model_path) return -EINVAL;
    memset(det, 0, sizeof(*det));

    ret = ocr_rknn_load(&det->rknn, model_path);
    if (ret != 0) return ret;
    if (det->rknn.input_num != 1 || det->rknn.output_num != 1) {
        LOG_E("detector requires exactly one input and one probability-map output");
        ret = -EINVAL;
        goto fail;
    }
    /* 输入尺寸以模型属性为准，不固定写死为 960 或 320；同样检查 batch=1、
     * RGB 三通道及 NCHW/NHWC 排列，否则后续连续内存无法按正确维度解释。 */
    ret = image_tensor_shape(&det->rknn.inputs[0], 3,
                             &det->input_w, &det->input_h);
    if (ret != 0) {
        LOG_E("unsupported detector input tensor contract");
        goto fail;
    }
    ret = output_map_shape(&det->rknn.outputs[0],
                           &det->output_w, &det->output_h);
    if (ret != 0) {
        LOG_E("unsupported detector output tensor contract; expected [1,1,H,W] or [1,H,W,1]");
        goto fail;
    }

    rknn_tensor_type input_type = (rknn_tensor_type)det->rknn.inputs[0].type;
    if (input_type != RKNN_TENSOR_UINT8 && input_type != RKNN_TENSOR_INT8 &&
        input_type != RKNN_TENSOR_FLOAT16 && input_type != RKNN_TENSOR_FLOAT32) {
        LOG_E("unsupported detector input type: %u", det->rknn.inputs[0].type);
        ret = -ENOTSUP;
        goto fail;
    }

    pixels = (size_t)det->input_w * (size_t)det->input_h;
    post_pixels = (size_t)det->output_w * (size_t)det->output_h;
    if (pixels > SIZE_MAX / (3u * sizeof(float)) ||
        post_pixels > SIZE_MAX / sizeof(*det->post_queue)) {
        ret = -EOVERFLOW;
        goto fail;
    }
    /* resize_buf 保存交错 RGB 字节，pre_buf 保存平面浮点数据，两者不是 DMA-BUF。
     * 后处理工作区按模型输出概率图尺寸分配，不一定等于输入图尺寸。 */
    det->resize_buf = malloc(pixels * 3u);
    det->pre_buf = malloc(pixels * 3u * sizeof(float));
    det->post_binary = malloc(post_pixels);
    det->post_queue = malloc(post_pixels * sizeof(*det->post_queue));
    det->post_capacity = post_pixels;
    if (!det->resize_buf || !det->pre_buf || !det->post_binary ||
        !det->post_queue) {
        ret = -ENOMEM;
        goto fail;
    }
    det->thresh = 0.3f;
    det->box_thresh = 0.6f;
    det->unclip_ratio = 1.6f;
    return 0;

fail:
    ocr_det_destroy(det);
    return ret;
}

/** @brief CPU 缩放得到 RGB，按模型输入类型选择字节输入或均值/方差归一化浮点输入。 */
static int preprocess(ocr_det_t *det, const ocr_buffer_t *buf,
                      const void **input, uint32_t *input_size,
                      rknn_tensor_type *input_type,
                      rknn_tensor_format *input_fmt)
{
    static const float mean[3] = {0.485f, 0.456f, 0.406f};
    static const float stddev[3] = {0.229f, 0.224f, 0.225f};
    size_t pixels = (size_t)det->input_w * (size_t)det->input_h;
    int ret = ocr_ai_resize_rgb(buf, det->resize_buf,
                                det->input_w, det->input_h);
    if (ret != 0) {
        LOG_E("detector preprocessing requires a mapped, tightly packed supported image: %d", ret);
        return ret;
    }

    rknn_tensor_type model_type = (rknn_tensor_type)det->rknn.inputs[0].type;
    /* 量化模型路径交给 RKNN 普通输入接口做转换：这里传入的源类型是 UINT8，
     * 不能因为模型内部是 INT8 就把 0..255 字节直接重解释为有符号值。
     * 均值/方差等预处理必须与转换模型时的配置一致，避免重复归一化。 */
    if (model_type == RKNN_TENSOR_UINT8 || model_type == RKNN_TENSOR_INT8) {
        *input = det->resize_buf;
        *input_size = (uint32_t)(pixels * 3u);
        *input_type = RKNN_TENSOR_UINT8;
        *input_fmt = RKNN_TENSOR_NHWC;
        return 0;
    }

    /* CPU 浮点路径同时做两件事：按 RGB 通道归一化，以及 HWC -> CHW 重排。
     * 原 i*3+c 是某像素的颜色；目标 c*pixels+i 把同一通道的全部像素排在一起。 */
    for (size_t i = 0; i < pixels; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            float value = det->resize_buf[i * 3u + (size_t)channel] / 255.0f;
            det->pre_buf[(size_t)channel * pixels + i] =
                (value - mean[channel]) / stddev[channel];
        }
    }
    *input = det->pre_buf;
    *input_size = (uint32_t)(pixels * 3u * sizeof(float));
    *input_type = RKNN_TENSOR_FLOAT32;
    *input_fmt = RKNN_TENSOR_NCHW;
    return 0;
}

/** @brief 检测一帧并把模型概率图转换成原图坐标的文本框；输入图像由调用者保活。 */
int ocr_det_run(ocr_det_t *det, const ocr_buffer_t *buf,
                ocr_text_box_list_t *boxes)
{
    const void *input;
    uint32_t input_size;
    rknn_tensor_type input_type;
    rknn_tensor_format input_fmt;
    void *output = NULL;
    uint32_t output_size = 0;
    int ret;

    if (!det || !det->rknn.initialized || !buf || !boxes) return -EINVAL;
    memset(boxes, 0, sizeof(*boxes));

    ret = preprocess(det, buf, &input, &input_size, &input_type, &input_fmt);
    if (ret != 0) return ret;
    ret = ocr_rknn_set_input_ex(&det->rknn, 0, input, input_size,
                                input_type, input_fmt);
    if (ret != 0) return ret;
    ret = ocr_rknn_run(&det->rknn);
    if (ret != 0) return ret;
    ret = ocr_rknn_get_output(&det->rknn, 0, &output, &output_size);
    if (ret != 0) return ret;

    /* 包装层以 want_float=1 取输出，即使模型内部量化类型不是 float，当前返回
     * 数组仍应是 n_elems 个 float，不能拿模型原始 attr.size 当成返回字节数。 */
    uint64_t expected = (uint64_t)det->rknn.outputs[0].n_elems * sizeof(float);
    if (expected != output_size || !output) {
        LOG_E("detector output size does not match queried float32 tensor");
        return -EMSGSIZE;
    }
    ret = ocr_db_postprocess_with_workspace(
        (const float *)output, det->output_w, det->output_h,
        det->thresh, det->box_thresh, det->unclip_ratio,
        det->post_binary, det->post_queue, det->post_capacity, boxes);
    if (ret != 0) return ret;

    /* 后处理的框坐标位于概率图，而调用方要在源图取字/叠加显示。
     * 本路径把整图直接缩放进模型，没有 letterbox，因此分别乘横/纵比例即可；
     * 若未来改成补边缩放，这里还必须扣除 padding，不能沿用当前变换。 */
    float scale_x = (float)buf->width / (float)det->output_w;
    float scale_y = (float)buf->height / (float)det->output_h;
    for (int i = 0; i < boxes->count; ++i) {
        for (int point = 0; point < OCR_BOX_POINTS; ++point) {
            boxes->boxes[i].x[point] *= scale_x;
            boxes->boxes[i].y[point] *= scale_y;
        }
    }
    return ocr_sort_boxes(boxes);
}

void ocr_det_destroy(ocr_det_t *det)
{
    if (!det) return;
    ocr_rknn_destroy(&det->rknn);
    free(det->resize_buf);
    free(det->pre_buf);
    free(det->post_binary);
    free(det->post_queue);
    memset(det, 0, sizeof(*det));
}
