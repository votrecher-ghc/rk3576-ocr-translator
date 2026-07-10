#ifndef OCR_DISPLAY_DRM_PLANE_H
#define OCR_DISPLAY_DRM_PLANE_H
/**
 * @file drm_plane.h
 * @brief Plane 管理：Plane0 底图 / Plane1 叠加
 */

#include "drm_device.h"

#define DRM_MAX_PLANES 4

/** Plane 类型 */
typedef enum {
    OCR_PLANE_OVERLAY = 0,
    OCR_PLANE_PRIMARY,
    OCR_PLANE_CURSOR,
} ocr_plane_type_t;

/** Plane 上下文 */
typedef struct {
    uint32_t plane_id;    /* Plane ID */
    uint32_t crtc_id;     /* 绑定的 CRTC ID */
    ocr_plane_type_t type;/* Plane 类型 */
    uint32_t fb_id;       /* 当前绑定的 framebuffer ID */
    int      in_use;      /* 是否已使用 */
} ocr_drm_plane_t;

/** Plane 管理器 */
typedef struct {
    ocr_drm_device_t *dev;
    ocr_drm_plane_t   planes[DRM_MAX_PLANES];
    int               plane_count;
    ocr_drm_plane_t  *primary;   /* Plane0 底图 */
    ocr_drm_plane_t  *overlay;   /* Plane1 叠加层 */
} ocr_drm_planes_t;

/**
 * @brief 枚举所有 Plane 并分类
 */
int ocr_drm_planes_init(ocr_drm_planes_t *planes, ocr_drm_device_t *dev);

/**
 * @brief 设置 Plane 的 framebuffer 与位置
 * @param[in] planes  Plane 管理器
 * @param[in] plane   目标 Plane
 * @param[in] fb_id   framebuffer ID
 * @param[in] x,y     显示位置
 * @param[in] w,h     显示宽高
 * @return 0=成功，负数=错误
 */
int ocr_drm_plane_set_fb(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                         uint32_t fb_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h);

/**
 * @brief 禁用 Plane
 */
int ocr_drm_plane_disable(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane);

/**
 * @brief 销毁 Plane 管理器
 */
void ocr_drm_planes_destroy(ocr_drm_planes_t *planes);

#endif /* OCR_DISPLAY_DRM_PLANE_H */
