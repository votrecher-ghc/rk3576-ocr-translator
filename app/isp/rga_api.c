/**
 * @file rga_api.c
 * @brief RGA 硬件加速封装实现
 */
#include "rga_api.h"
#include "log.h"

#include <limits.h>
#include <string.h>

/* Rockchip RGA 头文件 */
#include "im2d.h"

/* ocr 像素格式 → RGA 格式 */
static int fmt_to_rga(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:     return RK_FORMAT_YCbCr_420_SP;
        case OCR_FMT_NV16:     return RK_FORMAT_YCbCr_422_SP;
        case OCR_FMT_YUYV:     return RK_FORMAT_YUYV_422;
        case OCR_FMT_RGB565:   return RK_FORMAT_RGB_565;
        case OCR_FMT_RGB888:   return RK_FORMAT_RGB_888;
        /* OCR names follow DRM word order. On little-endian RK3576 those
         * correspond to the opposite byte-order names used by RGA. */
        case OCR_FMT_ARGB8888: return RK_FORMAT_BGRA_8888;
        case OCR_FMT_BGRA8888: return RK_FORMAT_ARGB_8888;
        default:               return -1;
    }
}

static uint32_t format_bytes_per_pixel(ocr_pixel_format_t format)
{
    switch (format) {
    case OCR_FMT_NV12:
    case OCR_FMT_NV16:     return 1;
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:   return 2;
    case OCR_FMT_RGB888:   return 3;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888: return 4;
    default:               return 0;
    }
}

static int wrap_buffer(const ocr_buffer_t *buffer, rga_buffer_t *rga_buffer)
{
    uint32_t bytes_per_pixel;
    uint64_t tight_stride;
    uint32_t byte_stride;
    uint32_t width_stride;
    uint32_t height_stride;
    int rga_format;

    if (!buffer || !rga_buffer || buffer->fd < 0 ||
        buffer->width == 0 || buffer->height == 0 ||
        buffer->width > INT_MAX || buffer->height > INT_MAX) {
        return -1;
    }
    bytes_per_pixel = format_bytes_per_pixel(buffer->format);
    rga_format = fmt_to_rga(buffer->format);
    if (bytes_per_pixel == 0 || rga_format < 0) return -1;

    tight_stride = (uint64_t)buffer->width * bytes_per_pixel;
    if (tight_stride > UINT32_MAX) return -1;
    byte_stride = buffer->plane_count > 0 ? buffer->strides[0] :
                  (uint32_t)tight_stride;
    if (byte_stride < tight_stride || byte_stride % bytes_per_pixel != 0) {
        return -1;
    }
    width_stride = byte_stride / bytes_per_pixel;
    height_stride = buffer->height;
    if (width_stride > INT_MAX) return -1;

    if (buffer->plane_count > 0) {
        if (buffer->offsets[0] != 0) return -1;
        if (buffer->format == OCR_FMT_NV12 || buffer->format == OCR_FMT_NV16) {
            if (buffer->plane_count != 2 || buffer->strides[1] != byte_stride ||
                buffer->offsets[1] == 0 ||
                buffer->offsets[1] % byte_stride != 0) {
                return -1;
            }
            height_stride = buffer->offsets[1] / byte_stride;
            if (height_stride < buffer->height || height_stride > INT_MAX) {
                return -1;
            }
        } else if (buffer->plane_count != 1) {
            return -1;
        }
    }

    memset(rga_buffer, 0, sizeof(*rga_buffer));
    *rga_buffer = wrapbuffer_fd_t(buffer->fd, (int)buffer->width,
                                  (int)buffer->height, (int)width_stride,
                                  (int)height_stride, rga_format);
    return rga_buffer->width > 0 && rga_buffer->height > 0 ? 0 : -1;
}

int ocr_rga_wrap_fd(int fd, uint32_t width, uint32_t height,
                    ocr_pixel_format_t format, void *handle)
{
    int rga_fmt = fmt_to_rga(format);
    if (rga_fmt < 0 || fd < 0 || width == 0 || height == 0 || !handle) return -1;

    rga_buffer_t *rga_buf = (rga_buffer_t *)handle;
    memset(rga_buf, 0, sizeof(*rga_buf));
    if (width > INT_MAX || height > INT_MAX) return -1;
    *rga_buf = wrapbuffer_fd_t(fd, (int)width, (int)height,
                               (int)width, (int)height, rga_fmt);
    if (rga_buf->width == 0) {
        LOG_E("wrapbuffer_fd 失败 fd=%d", fd);
        return -2;
    }
    return 0;
}

int ocr_rga_resize(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!src || !dst || src->fd < 0 || dst->fd < 0 ||
        src->width == 0 || src->height == 0 ||
        dst->width == 0 || dst->height == 0) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    IM_STATUS status = imresize_t(src_rga, dst_rga, 0.0, 0.0,
                                  IM_INTERP_DEFAULT, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imresize 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_crop(const ocr_buffer_t *src, ocr_buffer_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!src || !dst || w == 0 || h == 0 || x >= src->width || y >= src->height ||
        w > src->width - x || h > src->height - y ||
        (src->format == OCR_FMT_NV12 && ((x | y | w | h) & 1U)) ||
        ((src->format == OCR_FMT_NV16 || src->format == OCR_FMT_YUYV) &&
         ((x | w) & 1U))) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    im_rect src_rect = { .x = (int)x, .y = (int)y, .width = (int)w, .height = (int)h };

    IM_STATUS status = imcrop_t(src_rga, dst_rga, src_rect, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcrop 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_rotate(const ocr_buffer_t *src, ocr_buffer_t *dst, ocr_rga_rotate_t angle)
{
    if (!src || !dst) return -1;
    if ((angle == OCR_RGA_ROTATE_90 || angle == OCR_RGA_ROTATE_270) &&
        (dst->width != src->height || dst->height != src->width)) return -1;
    if ((angle == OCR_RGA_ROTATE_0 || angle == OCR_RGA_ROTATE_180) &&
        (dst->width != src->width || dst->height != src->height)) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    int transform;
    switch (angle) {
    case OCR_RGA_ROTATE_0:   transform = 0; break;
    case OCR_RGA_ROTATE_90:  transform = IM_HAL_TRANSFORM_ROT_90; break;
    case OCR_RGA_ROTATE_180: transform = IM_HAL_TRANSFORM_ROT_180; break;
    case OCR_RGA_ROTATE_270: transform = IM_HAL_TRANSFORM_ROT_270; break;
    default: return -1;
    }
    IM_STATUS status = transform == 0 ? imcopy_t(src_rga, dst_rga, 1) :
                                        imrotate_t(src_rga, dst_rga,
                                                   transform, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imrotate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_translate(const ocr_buffer_t *src, ocr_buffer_t *dst,
                      int32_t dx, int32_t dy)
{
    if (!src || !dst || src->width != dst->width || src->height != dst->height ||
        dx <= -(int32_t)src->width || dx >= (int32_t)dst->width ||
        dy <= -(int32_t)src->height || dy >= (int32_t)dst->height) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    IM_STATUS status = imtranslate_t(src_rga, dst_rga, dx, dy, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imtranslate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_cvtcolor(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!src || !dst || src->format == dst->format ||
        src->width != dst->width || src->height != dst->height) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (wrap_buffer(src, &src_rga) != 0) return -2;
    if (wrap_buffer(dst, &dst_rga) != 0) return -3;

    IM_STATUS status = imcvtcolor_t(src_rga, dst_rga,
                                    fmt_to_rga(src->format),
                                    fmt_to_rga(dst->format),
                                    IM_COLOR_SPACE_DEFAULT, 1);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcvtcolor 失败: %d", status);
        return -4;
    }
    return 0;
}
