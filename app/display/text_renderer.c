/**
 * @file text_renderer.c
 * @brief FreeType 文字渲染到 32 位 premultiplied ARGB 画布
 *
 * 屏幕文字层和拍照合成共用同一套排版与混合规则，保证照片上的译文与屏幕上看到的一致。
 * 每个实例拥有独立的 FreeType 库和字体 face，同一实例只能由一个线程使用。
 */
#include "text_renderer.h"
#include "log.h"

#include <stddef.h>
#include <string.h>

int ocr_text_renderer_init(ocr_text_renderer_t *renderer, const char *font_path,
                           int font_size)
{
    if (!renderer || !font_path || font_path[0] == '\0' || font_size <= 0) {
        return -1;
    }
    memset(renderer, 0, sizeof(*renderer));

    FT_Error ft_err = FT_Init_FreeType(&renderer->lib);
    if (ft_err) {
        LOG_E("FT_Init_FreeType 失败: %d", ft_err);
        renderer->lib = NULL;
        return -2;
    }

    ft_err = FT_New_Face(renderer->lib, font_path, 0, &renderer->face);
    if (ft_err) {
        LOG_E("FT_New_Face 加载字体 %s 失败: %d", font_path, ft_err);
        renderer->face = NULL;
        ocr_text_renderer_destroy(renderer);
        return -3;
    }

    ft_err = FT_Set_Pixel_Sizes(renderer->face, 0, (FT_UInt)font_size);
    if (ft_err) {
        LOG_E("FT_Set_Pixel_Sizes 失败: %d", ft_err);
        ocr_text_renderer_destroy(renderer);
        return -4;
    }

    renderer->font_size = font_size;
    renderer->initialized = 1;
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
 * 画布使用 premultiplied ARGB，与 DRM 的 Pre-multiplied blend mode 对齐。
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

/** @brief 把 FreeType 字形按覆盖率混合到画布，逐行寻址必须使用调用方给出的 pitch。 */
int ocr_text_renderer_draw(ocr_text_renderer_t *renderer, void *canvas,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           const char *text, int x, int y, uint32_t color)
{
    if (!renderer || !renderer->initialized || !renderer->face || !canvas ||
        !text || width == 0 || height == 0 || pitch < width * 4U) {
        return -1;
    }

    FT_Face face = renderer->face;
    int pen_x = x;
    int pen_y = y + renderer->font_size;
    const int origin_x = x;
    const int line_step = renderer->font_size + renderer->font_size / 4;
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
            pen_x += renderer->font_size * 2;
            previous = 0;
            continue;
        }

        FT_UInt glyph_index = FT_Get_Char_Index(face, codepoint);
        if (previous && glyph_index && FT_HAS_KERNING(face)) {
            FT_Vector kerning;
            if (FT_Get_Kerning(face, previous, glyph_index,
                               FT_KERNING_DEFAULT, &kerning) == 0) {
                pen_x += (int)(kerning.x >> 6);
            }
        }

        FT_Error ft_err = FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT);
        if (ft_err || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
            previous = 0;
            continue;
        }

        FT_GlyphSlot slot = face->glyph;
        const FT_Bitmap *bitmap = &slot->bitmap;
        for (unsigned int row = 0; row < bitmap->rows; ++row) {
            for (unsigned int col = 0; col < bitmap->width; ++col) {
                int px = pen_x + slot->bitmap_left + (int)col;
                int py = pen_y - slot->bitmap_top + (int)row;
                if (px < 0 || py < 0 ||
                    (uint32_t)px >= width || (uint32_t)py >= height) {
                    continue;
                }

                uint8_t coverage = glyph_coverage(bitmap, row, col);
                uint32_t *dst = (uint32_t *)
                    ((uint8_t *)canvas + (size_t)py * pitch + (size_t)px * 4U);
                blend_pixel(dst, color, coverage);
            }
        }

        pen_x += (int)(slot->advance.x >> 6);
        pen_y += (int)(slot->advance.y >> 6);
        previous = glyph_index;
    }
    return 0;
}

void ocr_text_renderer_destroy(ocr_text_renderer_t *renderer)
{
    if (!renderer) return;
    if (renderer->face) {
        FT_Done_Face(renderer->face);
        renderer->face = NULL;
    }
    if (renderer->lib) {
        FT_Done_FreeType(renderer->lib);
        renderer->lib = NULL;
    }
    renderer->font_size = 0;
    renderer->initialized = 0;
}
