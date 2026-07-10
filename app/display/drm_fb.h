#ifndef OCR_DISPLAY_DRM_FB_H
#define OCR_DISPLAY_DRM_FB_H
/**
 * @file drm_fb.h
 * @brief DRM framebuffer：DMA-BUF 绑定（drmPrimeFDToHandle + drmModeAddFB2）
 */

#include "buffer.h"
#include "drm_device.h"

/** DRM framebuffer 句柄 */
typedef struct {
    uint32_t fb_id;     /* DRM framebuffer ID */
    uint32_t handle;    /* GEM handle（drmPrimeFDToHandle 获得） */
    uint32_t width;     /* 宽 */
    uint32_t height;    /* 高 */
    ocr_pixel_format_t format; /* 像素格式 */
    int      dmabuf_fd; /* 绑定的 DMA-BUF fd */
} ocr_drm_fb_t;

/**
 * @brief 从 DMA-BUF fd 创建 DRM framebuffer
 * @param[in] dev   DRM 设备
 * @param[in] buf   缓冲区（含 fd/width/height/format）
 * @param[out] fb   framebuffer 句柄
 * @return 0=成功，负数=错误
 */
int ocr_drm_fb_create(ocr_drm_device_t *dev, const ocr_buffer_t *buf, ocr_drm_fb_t *fb);

/**
 * @brief 销毁 framebuffer（drmModeRmFB + gem close）
 */
int ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb);

/**
 * @brief pageflip：原子切换到指定 framebuffer
 * @param[in] dev     DRM 设备
 * @param[in] fb      目标 framebuffer
 * @param[in] vblank_cb vblank 回调（可为 NULL）
 * @return 0=成功，负数=错误
 */
int ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                        void (*vblank_cb)(int, uint32_t, uint32_t, void *),
                        void *user_data);

#endif /* OCR_DISPLAY_DRM_FB_H */
