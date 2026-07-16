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
    uint32_t handles[4];/* 每个格式平面的 GEM handle（NV12 两平面可相同） */
    uint32_t pitches[4];/* 每平面 stride */
    uint32_t offsets[4];/* 每平面在 DMA-BUF 中的偏移 */
    uint32_t plane_count;/* 格式平面数 */
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
 * @brief 使用显式 stride/offset 从 DMA-BUF 创建 framebuffer
 *
 * pitches/offsets 可传 NULL 使用紧密排列默认值。NV12/NV16 的两个格式平面
 * 共享同一个 DMA-BUF/GEM handle。
 */
int ocr_drm_fb_create_with_layout(ocr_drm_device_t *dev,
                                  const ocr_buffer_t *buf,
                                  const uint32_t pitches[4],
                                  const uint32_t offsets[4],
                                  ocr_drm_fb_t *fb);

/**
 * @brief 销毁 framebuffer（drmModeRmFB + gem close）
 */
int ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb);

/**
 * @brief pageflip：同步切换到指定 framebuffer
 * @param[in] dev     DRM 设备
 * @param[in] fb      目标 framebuffer
 * @param[in] vblank_cb vblank 回调（可为 NULL）
 * @return 0=成功，负数=错误
 *
 * 首帧会同步执行 modeset；后续帧使用 DRM_MODE_PAGE_FLIP_EVENT，并在
 * 收到该次 flip 的完成事件后返回。返回后可安全回收上一帧。
 */
int ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                        void (*vblank_cb)(int, uint32_t, uint32_t, void *),
                        void *user_data);

#endif /* OCR_DISPLAY_DRM_FB_H */
