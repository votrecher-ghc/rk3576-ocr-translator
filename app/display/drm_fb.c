/**
 * @file drm_fb.c
 * @brief DRM framebuffer 实现实现
 */
#include "drm_fb.h"
#include "log.h"

#include <string.h>
#include <errno.h>
#include <drm_fourcc.h>

/* ocr 像素格式 → DRM fourcc */
static uint32_t fmt_to_drm_fourcc(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:     return DRM_FORMAT_NV12;
        case OCR_FMT_NV16:     return DRM_FORMAT_NV16;
        case OCR_FMT_YUYV:     return DRM_FORMAT_YUYV;
        case OCR_FMT_RGB565:   return DRM_FORMAT_RGB565;
        case OCR_FMT_RGB888:   return DRM_FORMAT_RGB888;
        case OCR_FMT_ARGB8888: return DRM_FORMAT_ARGB8888;
        case OCR_FMT_BGRA8888: return DRM_FORMAT_BGRA8888;
        default:               return DRM_FORMAT_ARGB8888;
    }
}

int ocr_drm_fb_create(ocr_drm_device_t *dev, const ocr_buffer_t *buf, ocr_drm_fb_t *fb)
{
    if (!dev || !buf || !fb) return -1;
    memset(fb, 0, sizeof(*fb));

    /* DMA-BUF fd → GEM handle */
    int ret = drmPrimeFDToHandle(dev->fd, buf->fd, &fb->handle);
    if (ret < 0) {
        LOG_E("drmPrimeFDToHandle 失败: %s", strerror(errno));
        return -2;
    }

    /* 创建 framebuffer（drmModeAddFB2 支持 NV12 等多平面格式） */
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    handles[0] = fb->handle;

    /* 计算行跨距（简化：单平面格式按 bpp 计算，NV12 单独处理） */
    if (buf->format == OCR_FMT_NV12) {
        pitches[0] = buf->width;           /* Y 平面 */
        pitches[1] = buf->width;           /* UV 平面 */
        offsets[1] = buf->width * buf->height;
        handles[1] = fb->handle;
    } else {
        uint32_t bpp = (buf->format == OCR_FMT_RGB565) ? 16 :
                       (buf->format == OCR_FMT_RGB888) ? 24 : 32;
        pitches[0] = buf->width * (bpp / 8);
    }

    uint32_t fourcc = fmt_to_drm_fourcc(buf->format);
    ret = drmModeAddFB2(dev->fd, buf->width, buf->height, fourcc,
                        handles, pitches, offsets, &fb->fb_id, 0);
    if (ret < 0) {
        LOG_E("drmModeAddFB2 失败: %s", strerror(errno));
        /* 关闭 GEM handle */
        struct drm_gem_close close_arg = { .handle = fb->handle };
        drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
        return -3;
    }

    fb->width = buf->width;
    fb->height = buf->height;
    fb->format = buf->format;
    fb->dmabuf_fd = buf->fd;
    LOG_D("DRM FB 创建: id=%u %ux%u handle=%u", fb->fb_id, fb->width, fb->height, fb->handle);
    return 0;
}

int ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb)
{
    if (!dev || !fb) return -1;
    if (fb->fb_id) {
        drmModeRmFB(dev->fd, fb->fb_id);
        fb->fb_id = 0;
    }
    if (fb->handle) {
        struct drm_gem_close close_arg = { .handle = fb->handle };
        drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
        fb->handle = 0;
    }
    return 0;
}

int ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                        void (*vblank_cb)(int, uint32_t, uint32_t, void *),
                        void *user_data)
{
    if (!dev || !fb) return -1;
    uint32_t flags = DRM_MODE_PAGEFLIP_EVENT;
    if (vblank_cb) {
        /* TODO: 注册 vblank 事件回调（需配合 DRM 事件处理） */
        (void)vblank_cb; (void)user_data;
    }
    int ret = drmModePageFlip(dev->fd, dev->crtc->crtc_id, fb->fb_id, flags, user_data);
    if (ret < 0) {
        LOG_E("drmModePageFlip 失败: %s", strerror(errno));
        return -2;
    }
    return 0;
}
