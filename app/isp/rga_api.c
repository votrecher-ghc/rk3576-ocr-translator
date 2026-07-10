/**
 * @file rga_api.c
 * @brief RGA 硬件加速封装实现
 */
#include "rga_api.h"
#include "log.h"

#include <string.h>
#include <dlfcn.h>

/* Rockchip RGA 头文件 */
#include "im2d.h"
#include "RgaUtils.h"
#include "rga.h"

/* ocr 像素格式 → RGA 格式 */
static int fmt_to_rga(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:     return RK_FORMAT_YCbCr_420_SP;
        case OCR_FMT_NV16:     return RK_FORMAT_YCbCr_422_SP;
        case OCR_FMT_YUYV:     return RK_FORMAT_YCbCr_422_SP; /* TODO: 映射 */
        case OCR_FMT_RGB565:   return RK_FORMAT_RGB_565;
        case OCR_FMT_RGB888:   return RK_FORMAT_RGB_888;
        case OCR_FMT_ARGB8888: return RK_FORMAT_BGRA_8888;
        case OCR_FMT_BGRA8888: return RK_FORMAT_BGRA_8888;
        default:               return -1;
    }
}

int ocr_rga_wrap_fd(int fd, uint32_t width, uint32_t height,
                    ocr_pixel_format_t format, void *handle)
{
    int rga_fmt = fmt_to_rga(format);
    if (rga_fmt < 0 || fd < 0) return -1;

    rga_buffer_t *rga_buf = (rga_buffer_t *)handle;
    memset(rga_buf, 0, sizeof(*rga_buf));
    *rga_buf = wrapbuffer_fd(fd, width, height, rga_fmt);
    if (rga_buf->width == 0) {
        LOG_E("wrapbuffer_fd 失败 fd=%d", fd);
        return -2;
    }
    return 0;
}

int ocr_rga_resize(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!src || !dst) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (ocr_rga_wrap_fd(src->fd, src->width, src->height, src->format, &src_rga) != 0) return -2;
    if (ocr_rga_wrap_fd(dst->fd, dst->width, dst->height, dst->format, &dst_rga) != 0) return -3;

    IM_STATUS status = imresize(src_rga, dst_rga);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imresize 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_crop(const ocr_buffer_t *src, ocr_buffer_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!src || !dst) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (ocr_rga_wrap_fd(src->fd, src->width, src->height, src->format, &src_rga) != 0) return -2;
    if (ocr_rga_wrap_fd(dst->fd, dst->width, dst->height, dst->format, &dst_rga) != 0) return -3;

    im_rect src_rect = { .x = (int)x, .y = (int)y, .width = (int)w, .height = (int)h };
    im_rect dst_rect = { .x = 0, .y = 0, .width = (int)dst->width, .height = (int)dst->height };

    IM_STATUS status = imcrop(src_rga, dst_rga, src_rect, dst_rect);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcrop 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_rotate(const ocr_buffer_t *src, ocr_buffer_t *dst, ocr_rga_rotate_t angle)
{
    if (!src || !dst) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (ocr_rga_wrap_fd(src->fd, src->width, src->height, src->format, &src_rga) != 0) return -2;
    if (ocr_rga_wrap_fd(dst->fd, dst->width, dst->height, dst->format, &dst_rga) != 0) return -3;

    IM_STATUS status = imrotate(src_rga, dst_rga, (im_rotation)angle);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imrotate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_translate(const ocr_buffer_t *src, ocr_buffer_t *dst,
                      int32_t dx, int32_t dy)
{
    if (!src || !dst) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (ocr_rga_wrap_fd(src->fd, src->width, src->height, src->format, &src_rga) != 0) return -2;
    if (ocr_rga_wrap_fd(dst->fd, dst->width, dst->height, dst->format, &dst_rga) != 0) return -3;

    IM_STATUS status = imtranslate(src_rga, dst_rga, dx, dy);
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imtranslate 失败: %d", status);
        return -4;
    }
    return 0;
}

int ocr_rga_cvtcolor(const ocr_buffer_t *src, ocr_buffer_t *dst)
{
    if (!src || !dst) return -1;
    rga_buffer_t src_rga, dst_rga;
    if (ocr_rga_wrap_fd(src->fd, src->width, src->height, src->format, &src_rga) != 0) return -2;
    if (ocr_rga_wrap_fd(dst->fd, dst->width, dst->height, dst->format, &dst_rga) != 0) return -3;

    IM_STATUS status = imcvtcolor(src_rga, dst_rga,
                                  fmt_to_rga(src->format),
                                  fmt_to_rga(dst->format));
    if (status != IM_STATUS_SUCCESS) {
        LOG_E("imcvtcolor 失败: %d", status);
        return -4;
    }
    return 0;
}
