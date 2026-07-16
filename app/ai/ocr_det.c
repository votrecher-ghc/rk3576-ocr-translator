/** @file ocr_det.c @brief OCR detection preprocessing and DB decoding. */
#include "ocr_det.h"
#include "image_preproc.h"
#include "log.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    if (model_type == RKNN_TENSOR_UINT8 || model_type == RKNN_TENSOR_INT8) {
        *input = det->resize_buf;
        *input_size = (uint32_t)(pixels * 3u);
        *input_type = RKNN_TENSOR_UINT8;
        *input_fmt = RKNN_TENSOR_NHWC;
        return 0;
    }

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
