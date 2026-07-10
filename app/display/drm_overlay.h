#ifndef OCR_DISPLAY_DRM_OVERLAY_H
#define OCR_DISPLAY_DRM_OVERLAY_H
/**
 * @file drm_overlay.h
 * @brief 翻译结果叠加渲染（FreeType 文字渲染到 ARGB framebuffer）
 */

#include "drm_device.h"
#include "drm_plane.h"
#include <ft2build.h>
#include FT_FREETYPE_H

#define OVERLAY_MAX_TEXT 256

/** 叠加渲染上下文 */
typedef struct {
    ocr_drm_device_t *dev;
    ocr_drm_planes_t *planes;
    FT_Library        ft_lib;      /* FreeType 库 */
    FT_Face           ft_face;     /* 字体面 */
    int               font_size;   /* 字号 */
    uint32_t          width;       /* 叠加层宽 */
    uint32_t          height;      /* 叠加层高 */
    void             *pixels;      /* ARGB 像素缓冲（CPU 可写） */
    size_t            pixel_size;  /* 像素缓冲大小 */
    int               dmabuf_fd;   /* 叠加层 DMA-BUF fd */
    uint32_t          fb_id;       /* DRM FB ID */
    uint32_t          gem_handle;  /* GEM handle */
} ocr_drm_overlay_t;

/**
 * @brief 初始化叠加渲染
 * @param[in] overlay   叠加上下文
 * @param[in] dev       DRM 设备
 * @param[in] planes    Plane 管理器
 * @param[in] font_path 字体文件路径
 * @param[in] font_size 字号
 * @param[in] width     叠加层宽
 * @param[in] height    叠加层高
 * @return 0=成功，负数=错误
 */
int ocr_overlay_init(ocr_drm_overlay_t *overlay, ocr_drm_device_t *dev,
                     ocr_drm_planes_t *planes, const char *font_path,
                     int font_size, uint32_t width, uint32_t height);

/**
 * @brief 渲染文本到叠加层
 * @param[in] overlay  叠加上下文
 * @param[in] text     UTF-8 文本
 * @param[in] x,y      渲染起点坐标
 * @param[in] color    文字颜色（0xAARRGGBB）
 * @return 0=成功，负数=错误
 */
int ocr_overlay_render_text(ocr_drm_overlay_t *overlay, const char *text,
                            int x, int y, uint32_t color);

/**
 * @brief 清空叠加层
 */
int ocr_overlay_clear(ocr_drm_overlay_t *overlay);

/**
 * @brief 提交叠加层到 DRM（更新 Plane1）
 */
int ocr_overlay_commit(ocr_drm_overlay_t *overlay);

/**
 * @brief 销毁叠加渲染
 */
void ocr_overlay_destroy(ocr_drm_overlay_t *overlay);

#endif /* OCR_DISPLAY_DRM_OVERLAY_H */
