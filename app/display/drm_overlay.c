/**
 * @file drm_overlay.c
 * @brief DRM dumb buffer 上的 FreeType ARGB 文字叠加
 */
#include "drm_overlay.h"
#include "log.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int create_dumb_overlay(ocr_drm_overlay_t *overlay, int index)
{
    struct drm_mode_create_dumb create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.width = overlay->width;
    create_req.height = overlay->height;
    create_req.bpp = 32;

    if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) != 0) {
        LOG_E("DRM_IOCTL_MODE_CREATE_DUMB 失败: %s", strerror(errno));
        return -1;
    }
    overlay->gem_handle[index] = create_req.handle;
    overlay->pitch[index] = create_req.pitch;

    if (create_req.size == 0 || create_req.size > SIZE_MAX ||
        overlay->pitch[index] < overlay->width * 4U) {
        LOG_E("DRM dumb buffer 返回无效布局: pitch=%u size=%llu",
              overlay->pitch[index], (unsigned long long)create_req.size);
        return -2;
    }
    overlay->pixel_size[index] = (size_t)create_req.size;

    struct drm_mode_map_dumb map_req;
    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = overlay->gem_handle[index];
    if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
        LOG_E("DRM_IOCTL_MODE_MAP_DUMB 失败: %s", strerror(errno));
        return -3;
    }

    void *pixels = mmap(NULL, overlay->pixel_size[index], PROT_READ | PROT_WRITE,
                        MAP_SHARED, overlay->dev->fd, (off_t)map_req.offset);
    if (pixels == MAP_FAILED) {
        LOG_E("mmap DRM dumb buffer 失败: %s", strerror(errno));
        return -4;
    }
    overlay->pixels[index] = pixels;
    memset(overlay->pixels[index], 0, overlay->pixel_size[index]);

    uint32_t handles[4] = { overlay->gem_handle[index], 0, 0, 0 };
    uint32_t pitches[4] = { overlay->pitch[index], 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(overlay->dev->fd, overlay->width, overlay->height,
                      DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                      &overlay->fb_id[index], 0) != 0) {
        LOG_E("drmModeAddFB2(ARGB8888 dumb) 失败: %s", strerror(errno));
        return -5;
    }

    /* PRIME fd 仅用于可选的跨模块共享；scanout 本身直接使用 GEM handle。 */
    int prime_fd = -1;
    if (drmPrimeHandleToFD(overlay->dev->fd, overlay->gem_handle[index],
                           DRM_CLOEXEC | DRM_RDWR, &prime_fd) == 0) {
        overlay->dmabuf_fd[index] = prime_fd;
    } else {
        LOG_D("dumb buffer 不支持 PRIME 导出（非致命）: %s", strerror(errno));
    }
    return 0;
}

int ocr_overlay_init(ocr_drm_overlay_t *overlay, ocr_drm_device_t *dev,
                     ocr_drm_planes_t *planes, const char *font_path,
                     int font_size, uint32_t width, uint32_t height)
{
    if (!overlay || !dev || !dev->opened || dev->fd < 0 ||
        !planes || !planes->overlay ||
        !font_path || font_path[0] == '\0' || font_size <= 0 ||
        width == 0 || height == 0 || width > UINT32_MAX / 4U) {
        return -1;
    }
    if (!planes->overlay->supports_argb) return -2;

    memset(overlay, 0, sizeof(*overlay));
    overlay->dmabuf_fd[0] = -1;
    overlay->dmabuf_fd[1] = -1;
    overlay->active_index = -1;
    overlay->resource_active = 1;
    overlay->dev = dev;
    overlay->planes = planes;
    overlay->font_size = font_size;
    overlay->width = width;
    overlay->height = height;

    FT_Error ft_err = FT_Init_FreeType(&overlay->ft_lib);
    if (ft_err) {
        LOG_E("FT_Init_FreeType 失败: %d", ft_err);
        ocr_overlay_destroy(overlay);
        return -3;
    }

    ft_err = FT_New_Face(overlay->ft_lib, font_path, 0, &overlay->ft_face);
    if (ft_err) {
        LOG_E("FT_New_Face 加载字体 %s 失败: %d", font_path, ft_err);
        ocr_overlay_destroy(overlay);
        return -4;
    }

    ft_err = FT_Set_Pixel_Sizes(overlay->ft_face, 0, (FT_UInt)font_size);
    if (ft_err) {
        LOG_E("FT_Set_Pixel_Sizes 失败: %d", ft_err);
        ocr_overlay_destroy(overlay);
        return -5;
    }

    for (int i = 0; i < 2; ++i) {
        int ret = create_dumb_overlay(overlay, i);
        if (ret != 0) {
            ocr_overlay_destroy(overlay);
            return -6;
        }
    }

    overlay->initialized = 1;
    LOG_I("双缓冲叠加层: %ux%u pitch=%u/%u fb=%u/%u 字号=%d",
          width, height, overlay->pitch[0], overlay->pitch[1],
          overlay->fb_id[0], overlay->fb_id[1], font_size);
    return 0;
}

static int is_continuation(unsigned char c)
{
    return (c & 0xC0U) == 0x80U;
}

/* 严格 UTF-8 解码：支持 1~4 字节，拒绝过长编码、代理项和 >U+10FFFF。 */
static uint32_t utf8_next(const unsigned char **cursor)
{
    const unsigned char *p = *cursor;
    uint32_t cp;

    if (p[0] < 0x80U) {
        *cursor = p + 1;
        return p[0];
    }
    if (p[0] >= 0xC2U && p[0] <= 0xDFU && p[1] != 0 && is_continuation(p[1])) {
        cp = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        *cursor = p + 2;
        return cp;
    }
    if (p[0] >= 0xE0U && p[0] <= 0xEFU &&
        p[1] != 0 && p[2] != 0 && is_continuation(p[1]) && is_continuation(p[2]) &&
        !(p[0] == 0xE0U && p[1] < 0xA0U) &&
        !(p[0] == 0xEDU && p[1] >= 0xA0U)) {
        cp = ((uint32_t)(p[0] & 0x0FU) << 12) |
             ((uint32_t)(p[1] & 0x3FU) << 6) |
             (uint32_t)(p[2] & 0x3FU);
        *cursor = p + 3;
        return cp;
    }
    if (p[0] >= 0xF0U && p[0] <= 0xF4U &&
        p[1] != 0 && p[2] != 0 && p[3] != 0 &&
        is_continuation(p[1]) && is_continuation(p[2]) && is_continuation(p[3]) &&
        !(p[0] == 0xF0U && p[1] < 0x90U) &&
        !(p[0] == 0xF4U && p[1] >= 0x90U)) {
        cp = ((uint32_t)(p[0] & 0x07U) << 18) |
             ((uint32_t)(p[1] & 0x3FU) << 12) |
             ((uint32_t)(p[2] & 0x3FU) << 6) |
             (uint32_t)(p[3] & 0x3FU);
        *cursor = p + 4;
        return cp;
    }

    *cursor = p + 1;
    return 0xFFFDU;
}

static uint8_t glyph_coverage(const FT_Bitmap *bitmap,
                              unsigned int row, unsigned int col)
{
    int pitch = bitmap->pitch;
    const uint8_t *line;
    if (pitch >= 0) {
        line = bitmap->buffer + (size_t)row * (size_t)pitch;
    } else {
        line = bitmap->buffer +
               (size_t)(bitmap->rows - 1U - row) * (size_t)(-pitch);
    }

    if (bitmap->pixel_mode == FT_PIXEL_MODE_MONO) {
        return (line[col >> 3] & (0x80U >> (col & 7U))) ? 255U : 0U;
    }
    if (bitmap->pixel_mode == FT_PIXEL_MODE_GRAY) {
        uint32_t value = line[col];
        if (bitmap->num_grays > 1 && bitmap->num_grays != 256) {
            value = value * 255U / (bitmap->num_grays - 1U);
        }
        return (uint8_t)value;
    }
    if (bitmap->pixel_mode == FT_PIXEL_MODE_BGRA) {
        return line[col * 4U + 3U];
    }
    return 0;
}

/*
 * buffer 使用 premultiplied ARGB，与 DRM 的 Pre-multiplied blend mode 对齐。
 * color 参数仍是调用方熟悉的 straight 0xAARRGGBB。
 */
static void blend_pixel(uint32_t *dst, uint32_t color, uint8_t coverage)
{
    uint32_t ca = (color >> 24) & 0xFFU;
    uint32_t cr = (color >> 16) & 0xFFU;
    uint32_t cg = (color >> 8) & 0xFFU;
    uint32_t cb = color & 0xFFU;
    uint32_t sa = ca * coverage / 255U;
    if (sa == 0) return;

    uint32_t old = *dst;
    uint32_t da = (old >> 24) & 0xFFU;
    uint32_t dr = (old >> 16) & 0xFFU;
    uint32_t dg = (old >> 8) & 0xFFU;
    uint32_t db = old & 0xFFU;
    uint32_t inv = 255U - sa;

    uint32_t out_a = sa + da * inv / 255U;
    uint32_t out_r = cr * sa / 255U + dr * inv / 255U;
    uint32_t out_g = cg * sa / 255U + dg * inv / 255U;
    uint32_t out_b = cb * sa / 255U + db * inv / 255U;

    *dst = (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

int ocr_overlay_render_text(ocr_drm_overlay_t *overlay, const char *text,
                            int x, int y, uint32_t color)
{
    int draw;
    if (!overlay || !overlay->initialized || !text || !overlay->ft_face ||
        overlay->draw_index < 0 || overlay->draw_index > 1) {
        return -1;
    }
    draw = overlay->draw_index;
    if (!overlay->pixels[draw]) return -1;

    int pen_x = x;
    int pen_y = y + overlay->font_size;
    const int origin_x = x;
    const int line_step = overlay->font_size + overlay->font_size / 4;
    FT_UInt previous = 0;

    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor) {
        uint32_t codepoint = utf8_next(&cursor);
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            pen_x = origin_x;
            pen_y += line_step;
            previous = 0;
            continue;
        }
        if (codepoint == '\t') {
            pen_x += overlay->font_size * 2;
            previous = 0;
            continue;
        }

        FT_UInt glyph_index = FT_Get_Char_Index(overlay->ft_face, codepoint);
        if (previous && glyph_index && FT_HAS_KERNING(overlay->ft_face)) {
            FT_Vector kerning;
            if (FT_Get_Kerning(overlay->ft_face, previous, glyph_index,
                               FT_KERNING_DEFAULT, &kerning) == 0) {
                pen_x += (int)(kerning.x >> 6);
            }
        }

        FT_Error ft_err = FT_Load_Glyph(overlay->ft_face, glyph_index, FT_LOAD_DEFAULT);
        if (ft_err || FT_Render_Glyph(overlay->ft_face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            previous = 0;
            continue;
        }

        FT_GlyphSlot slot = overlay->ft_face->glyph;
        const FT_Bitmap *bitmap = &slot->bitmap;
        for (unsigned int row = 0; row < bitmap->rows; ++row) {
            for (unsigned int col = 0; col < bitmap->width; ++col) {
                int px = pen_x + slot->bitmap_left + (int)col;
                int py = pen_y - slot->bitmap_top + (int)row;
                if (px < 0 || py < 0 ||
                    (uint32_t)px >= overlay->width ||
                    (uint32_t)py >= overlay->height) {
                    continue;
                }

                uint8_t coverage = glyph_coverage(bitmap, row, col);
                uint32_t *dst = (uint32_t *)
                    ((uint8_t *)overlay->pixels[draw] +
                     (size_t)py * overlay->pitch[draw] +
                     (size_t)px * 4U);
                blend_pixel(dst, color, coverage);
            }
        }

        pen_x += (int)(slot->advance.x >> 6);
        pen_y += (int)(slot->advance.y >> 6);
        previous = glyph_index;
    }
    return 0;
}

int ocr_overlay_clear(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->initialized ||
        overlay->draw_index < 0 || overlay->draw_index > 1)
        return -1;
    int draw = overlay->draw_index;
    if (!overlay->pixels[draw] || overlay->pixel_size[draw] == 0) return -1;
    memset(overlay->pixels[draw], 0, overlay->pixel_size[draw]);
    return 0;
}

static int wait_overlay_vblank(ocr_drm_overlay_t *overlay)
{
    drmVBlank vblank;
    memset(&vblank, 0, sizeof(vblank));
    vblank.request.type = DRM_VBLANK_RELATIVE;
    vblank.request.sequence = 1;
#if defined(DRM_VBLANK_HIGH_CRTC_MASK) && defined(DRM_VBLANK_HIGH_CRTC_SHIFT)
    vblank.request.type = (drmVBlankSeqType)(
        vblank.request.type |
        (((unsigned int)overlay->dev->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) &
         DRM_VBLANK_HIGH_CRTC_MASK));
#else
    if (overlay->dev->crtc_index == 1) {
        vblank.request.type = (drmVBlankSeqType)
            (vblank.request.type | DRM_VBLANK_SECONDARY);
    } else if (overlay->dev->crtc_index > 1) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    for (;;) {
        if (drmWaitVBlank(overlay->dev->fd, &vblank) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

int ocr_overlay_commit(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->initialized || !overlay->planes ||
        !overlay->planes->overlay || overlay->draw_index < 0 ||
        overlay->draw_index > 1 || overlay->fb_id[overlay->draw_index] == 0) {
        return -1;
    }

    int next = overlay->draw_index;
    /* 只提交非 scanout buffer；下一帧再绘制刚刚退役的另一块 buffer。 */
    __sync_synchronize();
    int ret = ocr_drm_plane_set_fb_ex(
        overlay->planes, overlay->planes->overlay, overlay->fb_id[next],
        0, 0, overlay->width, overlay->height,
        0, 0, overlay->width, overlay->height);
    if (ret != 0) return ret;
    if (wait_overlay_vblank(overlay) != 0) {
        LOG_W("等待 overlay vblank 失败: %s", strerror(errno));
        /* SetPlane 已提交但切换时点未知，冻结 CPU 绘制以免覆盖 scanout。 */
        overlay->initialized = 0;
        overlay->active_index = -1;
        return -2;
    }
    overlay->active_index = next;
    overlay->draw_index = 1 - next;
    return 0;
}

void ocr_overlay_destroy(ocr_drm_overlay_t *overlay)
{
    if (!overlay) return;
    if (!overlay->resource_active) {
        memset(overlay, 0, sizeof(*overlay));
        overlay->dmabuf_fd[0] = -1;
        overlay->dmabuf_fd[1] = -1;
        overlay->active_index = -1;
        return;
    }

    if (overlay->planes && overlay->planes->overlay &&
        overlay->planes->overlay->in_use) {
        (void)ocr_drm_plane_disable(overlay->planes, overlay->planes->overlay);
    }

    int cleanup_failed = 0;
    for (int i = 0; i < 2; ++i) {
        if (overlay->dev && overlay->dev->opened &&
            overlay->dev->fd >= 0 && overlay->fb_id[i] != 0) {
            if (drmModeRmFB(overlay->dev->fd, overlay->fb_id[i]) != 0) {
                LOG_W("移除 overlay FB %u 失败: %s",
                      overlay->fb_id[i], strerror(errno));
                cleanup_failed = 1;
                continue;
            }
            overlay->fb_id[i] = 0;
        }
        if (overlay->pixels[i] && overlay->pixel_size[i] != 0) {
            munmap(overlay->pixels[i], overlay->pixel_size[i]);
            overlay->pixels[i] = NULL;
        }
        if (overlay->dmabuf_fd[i] >= 0) {
            close(overlay->dmabuf_fd[i]);
            overlay->dmabuf_fd[i] = -1;
        }
        if (overlay->dev && overlay->dev->opened &&
            overlay->dev->fd >= 0 && overlay->gem_handle[i] != 0) {
            struct drm_mode_destroy_dumb destroy_req;
            memset(&destroy_req, 0, sizeof(destroy_req));
            destroy_req.handle = overlay->gem_handle[i];
            if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                         &destroy_req) != 0) {
                LOG_W("DRM_IOCTL_MODE_DESTROY_DUMB 失败: %s", strerror(errno));
            }
            overlay->gem_handle[i] = 0;
        }
    }
    if (cleanup_failed) return;
    if (overlay->ft_face) {
        FT_Done_Face(overlay->ft_face);
        overlay->ft_face = NULL;
    }
    if (overlay->ft_lib) {
        FT_Done_FreeType(overlay->ft_lib);
        overlay->ft_lib = NULL;
    }

    memset(overlay, 0, sizeof(*overlay));
    overlay->dmabuf_fd[0] = -1;
    overlay->dmabuf_fd[1] = -1;
    overlay->active_index = -1;
}
