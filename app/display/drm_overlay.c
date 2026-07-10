/**
 * @file drm_overlay.c
 * @brief 翻译结果叠加渲染实现
 */
#include "drm_overlay.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <drm_fourcc.h>

int ocr_overlay_init(ocr_drm_overlay_t *overlay, ocr_drm_device_t *dev,
                     ocr_drm_planes_t *planes, const char *font_path,
                     int font_size, uint32_t width, uint32_t height)
{
    if (!overlay || !dev || !planes || !font_path) return -1;
    memset(overlay, 0, sizeof(*overlay));
    overlay->dev = dev;
    overlay->planes = planes;
    overlay->font_size = font_size;
    overlay->width = width;
    overlay->height = height;

    /* 初始化 FreeType */
    FT_Error err = FT_Init_FreeType(&overlay->ft_lib);
    if (err) {
        LOG_E("FT_Init_FreeType 失败: %d", err);
        return -2;
    }

    err = FT_New_Face(overlay->ft_lib, font_path, 0, &overlay->ft_face);
    if (err) {
        LOG_E("FT_New_Face 加载字体 %s 失败: %d", font_path, err);
        FT_Done_FreeType(overlay->ft_lib);
        return -3;
    }

    err = FT_Set_Pixel_Sizes(overlay->ft_face, 0, font_size);
    if (err) {
        LOG_W("FT_Set_Pixel_Sizes 失败: %d", err);
    }

    /* 分配 ARGB 像素缓冲（CPU 渲染） */
    overlay->pixel_size = (size_t)width * height * 4;
    overlay->pixels = calloc(1, overlay->pixel_size);
    if (!overlay->pixels) {
        LOG_E("叠加层像素缓冲分配失败");
        goto err_ft;
    }

    /* TODO: 分配 DMA-BUF 并导出 fd，drmModeAddFB2 创建 ARGB framebuffer */
    /* 当前简化：使用 CPU 像素缓冲，实际显示需通过 dumb buffer 或 dma-heap 分配 */
    LOG_I("叠加层初始化: %ux%u 字号=%d", width, height, font_size);
    return 0;

err_ft:
    FT_Done_Face(overlay->ft_face);
    FT_Done_FreeType(overlay->ft_lib);
    return -4;
}

int ocr_overlay_render_text(ocr_drm_overlay_t *overlay, const char *text,
                            int x, int y, uint32_t color)
{
    if (!overlay || !text) return -1;
    if (!overlay->ft_face || !overlay->pixels) return -2;

    /* 提取颜色分量 */
    uint8_t a = (color >> 24) & 0xFF;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    int pen_x = x;
    int pen_y = y + overlay->font_size;
    FT_GlyphSlot slot = overlay->ft_face->glyph;

    /* 遍历 UTF-8 字符串 */
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        /* 简单处理 ASCII，多字节 UTF-8 需完整解码 */
        uint32_t codepoint = 0;
        if (*p < 0x80) {
            codepoint = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            codepoint = (*p & 0x1F) << 6; p++;
            if (*p) { codepoint |= (*p & 0x3F); p++; }
        } else if ((*p & 0xF0) == 0xE0) {
            codepoint = (*p & 0x0F) << 12; p++;
            if (*p) { codepoint |= (*p & 0x3F) << 6; p++; }
            if (*p) { codepoint |= (*p & 0x3F); p++; }
        } else {
            codepoint = '?'; p++;
        }

        FT_Error err = FT_Load_Char(overlay->ft_face, codepoint, FT_LOAD_RENDER);
        if (err) continue;

        /* 将字形位图混合到像素缓冲 */
        for (unsigned int row = 0; row < slot->bitmap.rows; row++) {
            for (unsigned int col = 0; col < slot->bitmap.width; col++) {
                int px = pen_x + slot->bitmap_left + (int)col;
                int py = pen_y - slot->bitmap_top + (int)row;
                if (px < 0 || (uint32_t)px >= overlay->width ||
                    py < 0 || (uint32_t)py >= overlay->height) continue;

                uint8_t alpha = slot->bitmap.buffer[row * slot->bitmap.pitch + col];
                if (alpha == 0) continue;

                uint32_t *dst = (uint32_t *)overlay->pixels +
                                py * overlay->width + px;
                /* Alpha 混合（简化） */
                uint8_t out_a = (uint8_t)((int)a * alpha / 255);
                *dst = (out_a << 24) | (r << 16) | (g << 8) | b;
            }
        }

        pen_x += slot->advance.x >> 6;
        pen_y += slot->advance.y >> 6;
    }

    return 0;
}

int ocr_overlay_clear(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->pixels) return -1;
    memset(overlay->pixels, 0, overlay->pixel_size);
    return 0;
}

int ocr_overlay_commit(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->planes || !overlay->planes->overlay) return -1;

    /* TODO: 将 CPU 像素缓冲内容刷入 DMA-BUF，然后更新 overlay plane */
    /* 当前需先完成 DMA-BUF 分配与 FB 创建（见 init 中的 TODO） */

    /* 临时：使用 ocr_drm_plane_set_fb 更新（fb_id 需在 init 中创建） */
    if (overlay->fb_id == 0) {
        LOG_W("叠加层 FB 未创建，跳过 commit");
        return -2;
    }

    return ocr_drm_plane_set_fb(overlay->planes, overlay->planes->overlay,
                                overlay->fb_id, 0, 0,
                                overlay->width, overlay->height);
}

void ocr_overlay_destroy(ocr_drm_overlay_t *overlay)
{
    if (!overlay) return;
    if (overlay->fb_id && overlay->dev) {
        drmModeRmFB(overlay->dev->fd, overlay->fb_id);
    }
    if (overlay->gem_handle && overlay->dev) {
        struct drm_gem_close close_arg = { .handle = overlay->gem_handle };
        drmIoctl(overlay->dev->fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
    }
    if (overlay->dmabuf_fd >= 0) close(overlay->dmabuf_fd);
    free(overlay->pixels);
    if (overlay->ft_face) FT_Done_Face(overlay->ft_face);
    if (overlay->ft_lib) FT_Done_FreeType(overlay->ft_lib);
    memset(overlay, 0, sizeof(*overlay));
}
