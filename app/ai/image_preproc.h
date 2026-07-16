#ifndef OCR_AI_IMAGE_PREPROC_H
#define OCR_AI_IMAGE_PREPROC_H

/* Internal CPU fallback used when no model-specific RGA buffer contract exists. */

#include "buffer.h"
#include "ocr_postproc.h"

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

static inline uint8_t ocr_ai_clip_u8(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static inline int ocr_ai_buffer_required_size(const ocr_buffer_t *buf,
                                              size_t *required)
{
    size_t pixels;
    size_t bytes;

    if (!buf || !required || buf->width == 0 || buf->height == 0 ||
        (size_t)buf->width > SIZE_MAX / (size_t)buf->height) {
        return -EINVAL;
    }
    pixels = (size_t)buf->width * (size_t)buf->height;
    switch (buf->format) {
    case OCR_FMT_NV12:
        if ((buf->width & 1u) || (buf->height & 1u) ||
            pixels > SIZE_MAX - pixels / 2u) {
            return -EINVAL;
        }
        bytes = pixels + pixels / 2u;
        break;
    case OCR_FMT_NV16:
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:
        if ((buf->format == OCR_FMT_YUYV || buf->format == OCR_FMT_NV16) &&
            (buf->width & 1u)) {
            return -EINVAL;
        }
        if (pixels > SIZE_MAX / 2u) return -EOVERFLOW;
        bytes = pixels * 2u;
        break;
    case OCR_FMT_RGB888:
        if (pixels > SIZE_MAX / 3u) return -EOVERFLOW;
        bytes = pixels * 3u;
        break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888:
        if (pixels > SIZE_MAX / 4u) return -EOVERFLOW;
        bytes = pixels * 4u;
        break;
    default:
        return -ENOTSUP;
    }
    if (buf->plane_count > 0 && buf->strides[0] > 0) {
        uint32_t row_bytes;
        uint32_t rows = buf->height;
        switch (buf->format) {
        case OCR_FMT_RGB888: row_bytes = buf->width * 3u; break;
        case OCR_FMT_RGB565:
        case OCR_FMT_YUYV: row_bytes = buf->width * 2u; break;
        case OCR_FMT_ARGB8888:
        case OCR_FMT_BGRA8888: row_bytes = buf->width * 4u; break;
        default: row_bytes = buf->width; break;
        }
        if (buf->strides[0] < row_bytes) return -EINVAL;
        uint64_t end = (uint64_t)buf->offsets[0] +
                       (uint64_t)(rows - 1u) * buf->strides[0] + row_bytes;
        if (buf->format == OCR_FMT_NV12 || buf->format == OCR_FMT_NV16) {
            if (buf->plane_count < 2 || buf->strides[1] < buf->width)
                return -EINVAL;
            uint32_t chroma_rows = buf->format == OCR_FMT_NV12 ?
                                   buf->height / 2u : buf->height;
            uint64_t chroma_end = (uint64_t)buf->offsets[1] +
                (uint64_t)(chroma_rows - 1u) * buf->strides[1] + buf->width;
            if (chroma_end > end) end = chroma_end;
        }
        if (end > SIZE_MAX) return -EOVERFLOW;
        bytes = (size_t)end;
    }
    *required = bytes;
    return 0;
}

static inline void ocr_ai_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v,
                                     float rgb[3])
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    if (c < 0) c = 0;
    rgb[0] = ocr_ai_clip_u8((298 * c + 409 * e + 128) >> 8);
    rgb[1] = ocr_ai_clip_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgb[2] = ocr_ai_clip_u8((298 * c + 516 * d + 128) >> 8);
}

static inline int ocr_ai_read_rgb(const ocr_buffer_t *buf, int x, int y,
                                  float rgb[3])
{
    const uint8_t *data = (const uint8_t *)buf->mmap_addr;
    size_t width = buf->width;
    size_t stride0 = buf->strides[0];
    size_t offset0 = buf->offsets[0];
    if (stride0 == 0) {
        switch (buf->format) {
        case OCR_FMT_RGB888: stride0 = width * 3u; break;
        case OCR_FMT_RGB565:
        case OCR_FMT_YUYV: stride0 = width * 2u; break;
        case OCR_FMT_ARGB8888:
        case OCR_FMT_BGRA8888: stride0 = width * 4u; break;
        default: stride0 = width; break;
        }
    }

    switch (buf->format) {
    case OCR_FMT_RGB888: {
        const uint8_t *p = data + offset0 + (size_t)y * stride0 + (size_t)x * 3u;
        rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
        return 0;
    }
    case OCR_FMT_RGB565: {
        const uint8_t *p = data + offset0 + (size_t)y * stride0 + (size_t)x * 2u;
        uint16_t value = (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
        rgb[0] = (float)(((value >> 11) & 0x1Fu) * 255u / 31u);
        rgb[1] = (float)(((value >> 5) & 0x3Fu) * 255u / 63u);
        rgb[2] = (float)((value & 0x1Fu) * 255u / 31u);
        return 0;
    }
    case OCR_FMT_ARGB8888: {
        const uint8_t *p = data + offset0 + (size_t)y * stride0 + (size_t)x * 4u;
        rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0];
        return 0;
    }
    case OCR_FMT_BGRA8888: {
        const uint8_t *p = data + offset0 + (size_t)y * stride0 + (size_t)x * 4u;
        /* DRM BGRA8888 word order is A,R,G,B in little-endian memory. */
        rgb[0] = p[1]; rgb[1] = p[2]; rgb[2] = p[3];
        return 0;
    }
    case OCR_FMT_NV12: {
        size_t stride1 = buf->strides[1] ? buf->strides[1] : width;
        size_t offset1 = buf->plane_count >= 2 ? buf->offsets[1] : width * buf->height;
        size_t yy = offset0 + (size_t)y * stride0 + (size_t)x;
        size_t uv = offset1 + (size_t)(y / 2) * stride1 + (size_t)(x & ~1);
        ocr_ai_yuv_to_rgb(data[yy], data[uv], data[uv + 1u], rgb);
        return 0;
    }
    case OCR_FMT_NV16: {
        size_t stride1 = buf->strides[1] ? buf->strides[1] : width;
        size_t offset1 = buf->plane_count >= 2 ? buf->offsets[1] : width * buf->height;
        size_t yy = offset0 + (size_t)y * stride0 + (size_t)x;
        size_t uv = offset1 + (size_t)y * stride1 + (size_t)(x & ~1);
        ocr_ai_yuv_to_rgb(data[yy], data[uv], data[uv + 1u], rgb);
        return 0;
    }
    case OCR_FMT_YUYV: {
        size_t pair = offset0 + (size_t)y * stride0 + (size_t)(x & ~1) * 2u;
        uint8_t yy = data[pair + ((x & 1) ? 2u : 0u)];
        ocr_ai_yuv_to_rgb(yy, data[pair + 1u], data[pair + 3u], rgb);
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

static inline int ocr_ai_sample_rgb(const ocr_buffer_t *buf, float x, float y,
                                    float rgb[3])
{
    if (x < 0.0f) x = 0.0f;
    if (y < 0.0f) y = 0.0f;
    if (x > (float)(buf->width - 1u)) x = (float)(buf->width - 1u);
    if (y > (float)(buf->height - 1u)) y = (float)(buf->height - 1u);

    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1 < (int)buf->width ? x0 + 1 : x0;
    int y1 = y0 + 1 < (int)buf->height ? y0 + 1 : y0;
    float fx = x - x0;
    float fy = y - y0;
    float p00[3], p10[3], p01[3], p11[3];

    if (ocr_ai_read_rgb(buf, x0, y0, p00) != 0 ||
        ocr_ai_read_rgb(buf, x1, y0, p10) != 0 ||
        ocr_ai_read_rgb(buf, x0, y1, p01) != 0 ||
        ocr_ai_read_rgb(buf, x1, y1, p11) != 0) {
        return -ENOTSUP;
    }
    for (int c = 0; c < 3; ++c) {
        float top = p00[c] + (p10[c] - p00[c]) * fx;
        float bottom = p01[c] + (p11[c] - p01[c]) * fx;
        rgb[c] = top + (bottom - top) * fy;
    }
    return 0;
}

static inline int ocr_ai_validate_image(const ocr_buffer_t *buf)
{
    size_t required;
    int ret = ocr_ai_buffer_required_size(buf, &required);
    if (ret != 0) return ret;
    if (!buf->mmap_addr || buf->size < required) return -ENODATA;
    return 0;
}

static inline int ocr_ai_resize_rgb(const ocr_buffer_t *buf, uint8_t *dst,
                                    int dst_width, int dst_height)
{
    int ret;
    if (!dst || dst_width <= 0 || dst_height <= 0) return -EINVAL;
    ret = ocr_ai_validate_image(buf);
    if (ret != 0) return ret;

    for (int y = 0; y < dst_height; ++y) {
        float src_y = ((y + 0.5f) * buf->height / dst_height) - 0.5f;
        for (int x = 0; x < dst_width; ++x) {
            float src_x = ((x + 0.5f) * buf->width / dst_width) - 0.5f;
            float rgb[3];
            ret = ocr_ai_sample_rgb(buf, src_x, src_y, rgb);
            if (ret != 0) return ret;
            size_t offset = ((size_t)y * (size_t)dst_width + (size_t)x) * 3u;
            dst[offset] = ocr_ai_clip_u8((int)lroundf(rgb[0]));
            dst[offset + 1u] = ocr_ai_clip_u8((int)lroundf(rgb[1]));
            dst[offset + 2u] = ocr_ai_clip_u8((int)lroundf(rgb[2]));
        }
    }
    return 0;
}

static inline int ocr_ai_warp_quad_rgb(const ocr_buffer_t *buf,
                                       const ocr_text_box_t *box,
                                       uint8_t *dst,
                                       int dst_width, int dst_height)
{
    float x0, x1, x2, x3, y0, y1, y2, y3;
    float dx1, dx2, dx3, dy1, dy2, dy3;
    float a, b, c, d, e, f, g, h;
    int ret;

    if (!box || !box->valid || !dst || dst_width <= 0 || dst_height <= 0) {
        return -EINVAL;
    }
    ret = ocr_ai_validate_image(buf);
    if (ret != 0) return ret;
    for (int i = 0; i < OCR_BOX_POINTS; ++i) {
        if (!isfinite(box->x[i]) || !isfinite(box->y[i])) return -EINVAL;
    }

    x0 = box->x[0]; x1 = box->x[1]; x2 = box->x[2]; x3 = box->x[3];
    y0 = box->y[0]; y1 = box->y[1]; y2 = box->y[2]; y3 = box->y[3];
    float twice_area = x0 * y1 - y0 * x1 + x1 * y2 - y1 * x2 +
                       x2 * y3 - y2 * x3 + x3 * y0 - y3 * x0;
    if (fabsf(twice_area) < 2.0f) return -EINVAL;
    dx1 = x1 - x2; dx2 = x3 - x2; dx3 = x0 - x1 + x2 - x3;
    dy1 = y1 - y2; dy2 = y3 - y2; dy3 = y0 - y1 + y2 - y3;

    if (fabsf(dx3) < 1e-6f && fabsf(dy3) < 1e-6f) {
        g = h = 0.0f;
    } else {
        float denominator = dx1 * dy2 - dx2 * dy1;
        if (fabsf(denominator) < 1e-6f) return -EINVAL;
        g = (dx3 * dy2 - dx2 * dy3) / denominator;
        h = (dx1 * dy3 - dx3 * dy1) / denominator;
    }
    a = x1 - x0 + g * x1;
    b = x3 - x0 + h * x3;
    c = x0;
    d = y1 - y0 + g * y1;
    e = y3 - y0 + h * y3;
    f = y0;

    for (int y = 0; y < dst_height; ++y) {
        float v = (y + 0.5f) / dst_height;
        for (int x = 0; x < dst_width; ++x) {
            float u = (x + 0.5f) / dst_width;
            float denominator = g * u + h * v + 1.0f;
            float rgb[3];
            if (fabsf(denominator) < 1e-6f) return -EINVAL;
            float src_x = (a * u + b * v + c) / denominator;
            float src_y = (d * u + e * v + f) / denominator;
            ret = ocr_ai_sample_rgb(buf, src_x, src_y, rgb);
            if (ret != 0) return ret;
            size_t offset = ((size_t)y * (size_t)dst_width + (size_t)x) * 3u;
            dst[offset] = ocr_ai_clip_u8((int)lroundf(rgb[0]));
            dst[offset + 1u] = ocr_ai_clip_u8((int)lroundf(rgb[1]));
            dst[offset + 2u] = ocr_ai_clip_u8((int)lroundf(rgb[2]));
        }
    }
    return 0;
}

#endif /* OCR_AI_IMAGE_PREPROC_H */
