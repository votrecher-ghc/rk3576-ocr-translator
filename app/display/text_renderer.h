#ifndef OCR_DISPLAY_TEXT_RENDERER_H
#define OCR_DISPLAY_TEXT_RENDERER_H
/**
 * @file text_renderer.h
 * @brief FreeType 文字渲染到 32 位 premultiplied ARGB 画布
 *
 * 屏幕文字层和拍照合成共用同一套排版与混合规则，保证照片上的译文与屏幕上看到的一致。
 * 每个实例拥有独立的 FreeType 库和字体 face；FreeType face 不能跨线程共用，
 * 因此每个绘制线程各持一个实例，同一实例只能由一个线程使用。
 */

#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct {
    FT_Library lib; /**< 本实例初始化的 FreeType 库句柄，destroy 时在字体 face 之后释放。 */
    FT_Face    face; /**< 本实例加载的字体 face，承载字形及字号设置；destroy 释放。 */
    int        font_size; /**< FT_Set_Pixel_Sizes 使用的像素字号，也决定行高和文字起点基线。 */
    int        initialized; /**< 字体加载完成、可以绘制的标志；destroy 后为 0。 */
} ocr_text_renderer_t;

/**
 * @brief 加载字体并设置像素字号
 * @return 0=成功，-1=参数无效，-2=FreeType 初始化失败，-3=字体加载失败，-4=字号设置失败
 * @note 失败时已申请的 FreeType 资源会被释放，实例保持未初始化状态。
 */
int ocr_text_renderer_init(ocr_text_renderer_t *renderer, const char *font_path,
                           int font_size);

/**
 * @brief 把 UTF-8 文本按覆盖率混合到画布
 * @param[in,out] canvas 32 位像素画布，每像素为本机字节序的 premultiplied 0xAARRGGBB
 * @param[in] width,height 画布有效尺寸，超出部分的字形被裁掉
 * @param[in] pitch 画布每行字节数，可以大于 width*4
 * @param[in] x,y 文字左上角；第一行基线在 y + font_size
 * @param[in] color straight 0xAARRGGBB 文字颜色
 * @return 0=成功，-1=参数无效或实例未初始化
 * @note 支持 \\n 换行和 \\t 缩进，不自动折行；非法 UTF-8 字节按 U+FFFD 绘制。
 */
int ocr_text_renderer_draw(ocr_text_renderer_t *renderer, void *canvas,
                           uint32_t width, uint32_t height, uint32_t pitch,
                           const char *text, int x, int y, uint32_t color);

/** @brief 释放字体 face 和 FreeType 库；对未初始化或已销毁的实例可重复调用。 */
void ocr_text_renderer_destroy(ocr_text_renderer_t *renderer);

#endif /* OCR_DISPLAY_TEXT_RENDERER_H */
