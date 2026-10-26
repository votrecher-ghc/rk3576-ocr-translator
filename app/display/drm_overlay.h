#ifndef OCR_DISPLAY_DRM_OVERLAY_H
#define OCR_DISPLAY_DRM_OVERLAY_H
/**
 * @file drm_overlay.h
 * @brief 翻译结果叠加渲染（FreeType 文字渲染到 ARGB framebuffer）
 *
 * 文字叠加使用两块 ARGB8888 DRM dumb buffer：CPU 绘制后台缓冲，再提交 overlay plane。
 * 前后缓冲按提交和 vblank 时序交替使用；PRIME fd 仅供可选共享，扫描输出直接使用 framebuffer。
 */

#include "drm_device.h"
#include "drm_plane.h"
#include <stddef.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#define OVERLAY_MAX_TEXT 256

/** 叠加渲染上下文 */
typedef struct {
    ocr_drm_device_t *dev; /**< 借用的 DRM 设备，须在 overlay 销毁及双缓冲退出扫描后再关闭。 */
    ocr_drm_planes_t *planes; /**< 借用的 plane 管理器，使用其可选 overlay；本模块不销毁管理器。 */
    FT_Library        ft_lib; /**< 本实例初始化的 FreeType 库句柄，destroy 时在字体 face 之后释放。 */
    FT_Face           ft_face; /**< 本实例加载的字体 face，承载字形及字号设置；destroy 释放。 */
    int               font_size; /**< FT_Set_Pixel_Sizes 使用的像素字号，不是印刷 point。 */
    uint32_t          width; /**< 文字层有效宽度，屏幕像素，用于字形裁边和 dumb buffer 分配。 */
    uint32_t          height; /**< 文字层有效高度，屏幕像素，与视频层共同映射到显示区域。 */
    uint32_t          pitch[2]; /**< 两块 ARGB 缓冲各自的字节行跨度，内核可能加入对齐；逐行绘制必须使用。 */
    void             *pixels[2]; /**< 本模块 mmap 的两块 dumb buffer CPU 地址，destroy 时 munmap。 */
    size_t            pixel_size[2]; /**< 两块缓冲各自的完整映射长度，字节；可能大于 width*height*4。 */
    int               dmabuf_fd[2]; /**< 两块缓冲可选 PRIME 导出 fd，-1 表示未导出；本模块拥有并关闭。 */
    uint32_t          fb_id[2]; /**< 两块文字层 framebuffer ID，与同索引 pixels/gem_handle 配对；destroy 时 RmFB。 */
    uint32_t          gem_handle[2]; /**< 两块 CREATE_DUMB 返回的 GEM handle，由本模块销毁 dumb buffer。 */
    int               draw_index; /**< 下一次 CPU 绘制的后台缓冲下标 0/1；必须与当前扫描缓冲不同。 */
    int               active_index; /**< 已提交为扫描源的缓冲下标；-1 表示尚未提交或扫描状态未知。 */
    int               initialized; /**< 允许绘制/提交的标志；翻页时点未知时可置 0，但仍可能需要清理资源。 */
    int               resource_active; /**< 已进入资源生命周期的标记；即使 initialized=0 也据此继续清理已申请资源。 */
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

/** @brief 提交非活动缓冲到 Plane1，等待 vblank 后交换双缓冲。 */
int ocr_overlay_commit(ocr_drm_overlay_t *overlay);

/**
 * @brief 销毁叠加渲染
 */
void ocr_overlay_destroy(ocr_drm_overlay_t *overlay);

#endif /* OCR_DISPLAY_DRM_OVERLAY_H */
