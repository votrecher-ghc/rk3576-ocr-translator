/** @file capture_session.c @brief Key-triggered archive of live OCR results.
 *
 * 按键只登记拍照请求；实时 OCR 线程识别完一帧后取走请求，把这一帧叠加译文后编码为
 * JPEG，并写入 result.txt / result.json。模型只由 OCR 线程使用，按键线程从不推理。
 */
#include "capture_session.h"
#include "log.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

typedef struct {
    struct jpeg_error_mgr base; /**< 内嵌 libjpeg 错误管理器，须保持首成员以由通用指针还原本扩展上下文。 */
    jmp_buf jump; /**< 本次 JPEG 编码的 setjmp 恢复点，错误回调 longjmp 到清理路径，非线程同步对象。 */
} jpeg_error_ctx_t;

/** @brief 将 libjpeg 致命错误转回本函数的清理路径，避免默认处理直接终止整个进程。 */
static void jpeg_error_exit(j_common_ptr cinfo)
{
    jpeg_error_ctx_t *error = (jpeg_error_ctx_t *)cinfo->err;
    longjmp(error->jump, 1);
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3])
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    if (c < 0) c = 0;
    rgb[0] = clamp_u8((298 * c + 409 * e + 128) >> 8);
    rgb[1] = clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
    rgb[2] = clamp_u8((298 * c + 516 * d + 128) >> 8);
}

/** @brief 按源图实际 stride 和 plane offset 生成一行 RGB，供 JPEG 编码逐行消费。 */
static int make_rgb_row(const ocr_buffer_t *buf, uint32_t row, uint8_t *rgb)
{
    const uint8_t *base = buf->mmap_addr;
    uint32_t stride = buf->strides[0];
    if (!base || row >= buf->height || !rgb || stride == 0) return -1;

    if (buf->format == OCR_FMT_RGB888) {
        memcpy(rgb, base + buf->offsets[0] + (size_t)row * stride,
               (size_t)buf->width * 3U);
        return 0;
    }
    if (buf->format == OCR_FMT_NV12 && buf->plane_count >= 2) {
        const uint8_t *y_plane = base + buf->offsets[0] + (size_t)row * stride;
        const uint8_t *uv_plane = base + buf->offsets[1] +
                                  (size_t)(row / 2U) * buf->strides[1];
        for (uint32_t x = 0; x < buf->width; ++x)
            yuv_to_rgb(y_plane[x], uv_plane[x & ~1U], uv_plane[(x & ~1U) + 1U],
                       rgb + (size_t)x * 3U);
        return 0;
    }
    if (buf->format == OCR_FMT_YUYV) {
        const uint8_t *src = base + buf->offsets[0] + (size_t)row * stride;
        for (uint32_t x = 0; x < buf->width; x += 2U) {
            uint8_t u = src[1], v = src[3];
            yuv_to_rgb(src[0], u, v, rgb + (size_t)x * 3U);
            if (x + 1U < buf->width)
                yuv_to_rgb(src[2], u, v, rgb + (size_t)(x + 1U) * 3U);
            src += 4;
        }
        return 0;
    }
    if (buf->format == OCR_FMT_ARGB8888) {
        const uint8_t *src = base + buf->offsets[0] + (size_t)row * stride;
        for (uint32_t x = 0; x < buf->width; ++x) {
            /* DRM little-endian ARGB8888 exposes B,G,R,A. */
            rgb[x * 3U + 0U] = src[x * 4U + 2U];
            rgb[x * 3U + 1U] = src[x * 4U + 1U];
            rgb[x * 3U + 2U] = src[x * 4U + 0U];
        }
        return 0;
    }
    if (buf->format == OCR_FMT_BGRA8888) {
        const uint8_t *src = base + buf->offsets[0] + (size_t)row * stride;
        for (uint32_t x = 0; x < buf->width; ++x) {
            /* DRM little-endian BGRA8888 exposes A,R,G,B. */
            rgb[x * 3U + 0U] = src[x * 4U + 1U];
            rgb[x * 3U + 1U] = src[x * 4U + 2U];
            rgb[x * 3U + 2U] = src[x * 4U + 3U];
        }
        return 0;
    }
    return -2;
}

/*
 * 把屏幕文字层同款的 premultiplied ARGB 图层叠到一行 RGB 上，公式与显示控制器的
 * Pre-multiplied 混合一致：out = layer + base * (1 - alpha)，所以照片与屏幕看到的一样。
 */
static void composite_row(uint8_t *rgb, const uint32_t *layer, uint32_t width)
{
    for (uint32_t x = 0; x < width; ++x) {
        uint32_t px = layer[x];
        uint32_t alpha = px >> 24;
        if (alpha == 0) continue;
        uint32_t inv = 255U - alpha;
        uint8_t *out = rgb + (size_t)x * 3U;
        out[0] = (uint8_t)(((px >> 16) & 0xFFU) + out[0] * inv / 255U);
        out[1] = (uint8_t)(((px >> 8) & 0xFFU) + out[1] * inv / 255U);
        out[2] = (uint8_t)((px & 0xFFU) + out[2] * inv / 255U);
    }
}

/**
 * @brief 逐行把画面转成 RGB、叠加可选文字图层并编码 JPEG，只分配一行临时内存
 * @param[in] layer 与画面同尺寸、行宽 width 的 premultiplied ARGB 图层；NULL 表示不叠字
 * @note 调用者负责包围源图的 CPU 读同步。
 */
static int encode_jpeg(const ocr_buffer_t *buf, const uint32_t *layer,
                       const char *path, int quality)
{
    if (!buf || !path || !*path || buf->width == 0 || buf->height == 0) return -1;
    if (!buf->mmap_addr) return -2;

    FILE *output = fopen(path, "wb");
    if (!output) return -3;
    uint8_t *row = malloc((size_t)buf->width * 3U);
    if (!row) {
        fclose(output);
        return -4;
    }

    struct jpeg_compress_struct cinfo;
    jpeg_error_ctx_t error;
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&cinfo);
        free(row);
        fclose(output);
        return -5;
    }

    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, output);
    cinfo.image_width = buf->width;
    cinfo.image_height = buf->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        if (make_rgb_row(buf, cinfo.next_scanline, row) != 0) {
            jpeg_abort_compress(&cinfo);
            jpeg_destroy_compress(&cinfo);
            free(row);
            fclose(output);
            return -6;
        }
        if (layer)
            composite_row(row, layer + (size_t)cinfo.next_scanline * buf->width,
                          buf->width);
        JSAMPROW scanline = row;
        (void)jpeg_write_scanlines(&cinfo, &scanline, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    return fclose(output) == 0 ? 0 : -7;
}

int ocr_capture_session_init(ocr_capture_session_t *sess, ocr_archiver_t *arch,
                             const char *font_path, int font_size)
{
    if (!sess || !arch) return -1;
    memset(sess, 0, sizeof(*sess));
    sess->archiver = arch;
    atomic_init(&sess->pending, 0);
    sess->jpeg_quality = 90;
    strcpy(sess->src_lang, "en");
    strcpy(sess->tgt_lang, "zh");
    /* 独立于屏幕文字层的字体实例：save 在 OCR 线程执行，FreeType face 不能与显示线程共用。 */
    if (ocr_text_renderer_init(&sess->text, font_path, font_size) != 0)
        LOG_W("拍照合成字体加载失败，保存的照片将不带译文");
    sess->initialized = 1;
    return 0;
}

int ocr_capture_session_configure(ocr_capture_session_t *sess,
                                  const char *src_lang, const char *tgt_lang,
                                  int jpeg_quality)
{
    if (!sess || !src_lang || !tgt_lang || !*src_lang || !*tgt_lang ||
        jpeg_quality < 1 || jpeg_quality > 100) return -1;
    snprintf(sess->src_lang, sizeof(sess->src_lang), "%s", src_lang);
    snprintf(sess->tgt_lang, sizeof(sess->tgt_lang), "%s", tgt_lang);
    sess->jpeg_quality = jpeg_quality;
    return 0;
}

void ocr_capture_session_request(ocr_capture_session_t *sess)
{
    if (!sess || !sess->initialized) return;
    /* 一次识别完成前的多次按键合并成一张照片，按键线程不等待、不阻塞。 */
    if (atomic_exchange_explicit(&sess->pending, 1, memory_order_acq_rel))
        LOG_I("上一次拍照请求尚未完成，本次按键已合并");
    else
        LOG_I("收到拍照请求，将保存下一次识别完成的画面");
}

int ocr_capture_session_take_request(ocr_capture_session_t *sess)
{
    if (!sess || !sess->initialized) return 0;
    return atomic_exchange_explicit(&sess->pending, 0, memory_order_acq_rel) != 0;
}

static void append_line(char *dst, size_t capacity, const char *line)
{
    size_t used = strlen(dst);
    if (used >= capacity - 1) return;
    (void)snprintf(dst + used, capacity - used, "%s%s", used ? "\n" : "",
                   line ? line : "");
}

/** @brief 先画字再编码 JPEG，随后写入文本和 JSON；所有路径最终释放临时内存。 */
int ocr_capture_session_save(ocr_capture_session_t *sess, const ocr_buffer_t *frame,
                             const ocr_text_box_list_t *boxes,
                             char (*texts)[OCR_MAX_TEXT_LEN],
                             char (*translated)[OCR_MAX_TEXT_LEN],
                             const ocr_capture_label_t *labels, int label_count,
                             uint32_t color)
{
    if (!sess || !sess->initialized || !frame || !boxes || boxes->count < 0 ||
        (boxes->count > 0 && (!texts || !translated)) ||
        label_count < 0 || (label_count > 0 && !labels) ||
        frame->width == 0 || frame->height == 0 || frame->width > UINT32_MAX / 4U) {
        return -1;
    }

    ocr_archive_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    if (ocr_archiver_create(sess->archiver, &entry) != 0) return -2;
    snprintf(entry.src_lang, sizeof(entry.src_lang), "%s", sess->src_lang);
    snprintf(entry.tgt_lang, sizeof(entry.tgt_lang), "%s", sess->tgt_lang);

    int result = -4;
    uint32_t *layer = NULL;
    char **text_ptrs = NULL;
    char **trans_ptrs = NULL;
    ocr_archive_box_t *archive_boxes = NULL;
    char *all_text = NULL;
    char *all_trans = NULL;

    /* 与屏幕文字层同格式的透明画布：先在上面画字，编码时逐行叠到画面上。 */
    if (sess->text.initialized && label_count > 0) {
        layer = calloc((size_t)frame->width * frame->height, sizeof(*layer));
        if (!layer) goto done;
        for (int i = 0; i < label_count; ++i) {
            if (!labels[i].text) continue;
            (void)ocr_text_renderer_draw(&sess->text, layer, frame->width,
                                         frame->height, frame->width * 4U,
                                         labels[i].text, labels[i].x,
                                         labels[i].y, color);
        }
    }
    if (encode_jpeg(frame, layer, entry.image_path, sess->jpeg_quality) != 0) {
        result = -3;
        goto done;
    }

    size_t rows = boxes->count > 0 ? (size_t)boxes->count : 1U;
    text_ptrs = calloc(rows, sizeof(*text_ptrs));
    trans_ptrs = calloc(rows, sizeof(*trans_ptrs));
    archive_boxes = calloc(rows, sizeof(*archive_boxes));
    all_text = calloc(1, ARCHIVE_MAX_TEXT);
    all_trans = calloc(1, ARCHIVE_MAX_TEXT);
    if (!text_ptrs || !trans_ptrs || !archive_boxes || !all_text || !all_trans)
        goto done;

    for (int i = 0; i < boxes->count; ++i) {
        text_ptrs[i] = texts[i];
        trans_ptrs[i] = translated[i];
        memcpy(archive_boxes[i].x, boxes->boxes[i].x, sizeof(archive_boxes[i].x));
        memcpy(archive_boxes[i].y, boxes->boxes[i].y, sizeof(archive_boxes[i].y));
        archive_boxes[i].score = boxes->boxes[i].score;
        archive_boxes[i].valid = boxes->boxes[i].valid;
        append_line(all_text, ARCHIVE_MAX_TEXT, texts[i]);
        append_line(all_trans, ARCHIVE_MAX_TEXT, translated[i]);
    }

    if (ocr_archiver_save_text(&entry, all_text, all_trans) != 0 ||
        ocr_archiver_save_json(&entry, archive_boxes, text_ptrs, trans_ptrs,
                               boxes->count) != 0) {
        result = -5;
        goto done;
    }
    result = 0;
    LOG_I("拍照已保存: %s（%d 个文本框）", entry.dir, boxes->count);

done:
    free(all_trans);
    free(all_text);
    free(archive_boxes);
    free(trans_ptrs);
    free(text_ptrs);
    free(layer);
    return result;
}

void ocr_capture_session_destroy(ocr_capture_session_t *sess)
{
    if (!sess) return;
    ocr_text_renderer_destroy(&sess->text);
    memset(sess, 0, sizeof(*sess));
}

/** @brief 只在拍照键按下时登记请求，释放事件不重复拍照。 */
void ocr_capture_session_on_key(ocr_key_event_t event, ocr_key_state_t state,
                                void *user)
{
    if (event == KEY_EVENT_CAMERA && state == KEY_STATE_PRESSED)
        ocr_capture_session_request((ocr_capture_session_t *)user);
}
