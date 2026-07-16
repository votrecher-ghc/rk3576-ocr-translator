/** @file capture_session.c @brief Synchronous photo OCR/archive workflow. */
#include "capture_session.h"
#include "zerocopy.h"
#include "log.h"

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jpeglib.h>

typedef struct {
    struct jpeg_error_mgr base;
    jmp_buf jump;
} jpeg_error_ctx_t;

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

static int encode_jpeg(ocr_buffer_t *buf, const char *path, int quality)
{
    if (!buf || !path || !*path || buf->width == 0 || buf->height == 0) return -1;
    if (!buf->mmap_addr && ocr_buffer_mmap(buf) != 0) return -2;

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
        JSAMPROW scanline = row;
        (void)jpeg_write_scanlines(&cinfo, &scanline, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(row);
    return fclose(output) == 0 ? 0 : -7;
}

int ocr_capture_session_init(ocr_capture_session_t *sess,
                             ocr_v4l2_capture_t *cap,
                             ocr_det_t *det, ocr_rec_t *rec,
                             ocr_translator_t *trs, ocr_archiver_t *arch)
{
    if (!sess || !cap || !det || !rec) return -1;
    memset(sess, 0, sizeof(*sess));
    sess->capture = cap;
    sess->det = det;
    sess->rec = rec;
    sess->translator = trs;
    sess->archiver = arch;
    sess->state = CAPTURE_SESSION_IDLE;
    atomic_init(&sess->busy, 0);
    sess->jpeg_quality = 90;
    strcpy(sess->src_lang, "en");
    strcpy(sess->tgt_lang, "zh");
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

static void append_line(char *dst, size_t capacity, const char *line)
{
    size_t used = strlen(dst);
    if (used >= capacity - 1) return;
    (void)snprintf(dst + used, capacity - used, "%s%s", used ? "\n" : "",
                   line ? line : "");
}

int ocr_capture_session_trigger(ocr_capture_session_t *sess)
{
    if (!sess) return -1;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&sess->busy, &expected, 1)) {
        LOG_W("capture session is already busy");
        return 1;
    }

    int result = -2;
    ocr_buffer_t *frame = NULL;
    char (*texts)[OCR_MAX_TEXT_LEN] = NULL;
    char (*translated)[OCR_MAX_TEXT_LEN] = NULL;
    char **text_ptrs = NULL;
    char **trans_ptrs = NULL;
    ocr_archive_box_t *archive_boxes = NULL;
    char *all_text = NULL;
    char *all_trans = NULL;
    ocr_archive_entry_t entry;
    int sync_started = 0;
    memset(&entry, 0, sizeof(entry));

    sess->state = CAPTURE_SESSION_CAPTURING;
    frame = ocr_v4l2_get_latest(sess->capture);
    if (!frame) goto done;
    if (frame->fd >= 0) {
        if (ocr_zerocopy_sync(frame->fd, 0) != 0) {
            result = -3;
            goto done;
        }
        sync_started = 1;
    }

    if (!sess->archiver || ocr_archiver_create(sess->archiver, &entry) != 0)
        goto done;
    snprintf(entry.src_lang, sizeof(entry.src_lang), "%s", sess->src_lang);
    snprintf(entry.tgt_lang, sizeof(entry.tgt_lang), "%s", sess->tgt_lang);

    sess->state = CAPTURE_SESSION_SAVING;
    if (encode_jpeg(frame, entry.image_path, sess->jpeg_quality) != 0) {
        result = -3;
        goto done;
    }

    sess->state = CAPTURE_SESSION_OCR;
    ocr_text_box_list_t boxes;
    memset(&boxes, 0, sizeof(boxes));
    if (ocr_det_run(sess->det, frame, &boxes) != 0) {
        result = -4;
        goto done;
    }

    size_t rows = boxes.count > 0 ? (size_t)boxes.count : 1U;
    texts = calloc(rows, sizeof(*texts));
    translated = calloc(rows, sizeof(*translated));
    text_ptrs = calloc(rows, sizeof(*text_ptrs));
    trans_ptrs = calloc(rows, sizeof(*trans_ptrs));
    archive_boxes = calloc(rows, sizeof(*archive_boxes));
    all_text = calloc(1, ARCHIVE_MAX_TEXT);
    all_trans = calloc(1, ARCHIVE_MAX_TEXT);
    if (!texts || !translated || !text_ptrs || !trans_ptrs || !archive_boxes ||
        !all_text || !all_trans) {
        result = -5;
        goto done;
    }

    for (int i = 0; i < boxes.count; ++i) {
        text_ptrs[i] = texts[i];
        trans_ptrs[i] = translated[i];
        memcpy(archive_boxes[i].x, boxes.boxes[i].x, sizeof(archive_boxes[i].x));
        memcpy(archive_boxes[i].y, boxes.boxes[i].y, sizeof(archive_boxes[i].y));
        archive_boxes[i].score = boxes.boxes[i].score;
        archive_boxes[i].valid = boxes.boxes[i].valid;
    }
    if (boxes.count > 0 &&
        ocr_rec_run_batch(sess->rec, frame, &boxes, text_ptrs) != 0) {
        result = -6;
        goto done;
    }

    sess->state = CAPTURE_SESSION_TRANSLATING;
    for (int i = 0; i < boxes.count; ++i) {
        if (sess->translator && sess->translator->initialized) {
            if (translator_translate(sess->translator, texts[i], translated[i],
                                     OCR_MAX_TEXT_LEN) != 0) {
                translated[i][0] = '\0';
            }
        }
        append_line(all_text, ARCHIVE_MAX_TEXT, texts[i]);
        append_line(all_trans, ARCHIVE_MAX_TEXT, translated[i]);
    }

    sess->state = CAPTURE_SESSION_SAVING;
    if (ocr_archiver_save_text(&entry, all_text, all_trans) != 0 ||
        ocr_archiver_save_json(&entry, archive_boxes, text_ptrs, trans_ptrs,
                               boxes.count) != 0) {
        result = -7;
        goto done;
    }
    result = 0;

done:
    if (sync_started && ocr_zerocopy_sync(frame->fd, 1) != 0 && result == 0)
        result = -8;
    if (frame) (void)ocr_buffer_unref(frame);
    free(all_trans);
    free(all_text);
    free(archive_boxes);
    free(trans_ptrs);
    free(text_ptrs);
    free(translated);
    free(texts);
    sess->state = result == 0 ? CAPTURE_SESSION_DONE : CAPTURE_SESSION_IDLE;
    atomic_store(&sess->busy, 0);
    return result;
}

void ocr_capture_session_on_key(ocr_key_event_t event, ocr_key_state_t state,
                                void *user)
{
    if (event == KEY_EVENT_CAMERA && state == KEY_STATE_PRESSED)
        (void)ocr_capture_session_trigger((ocr_capture_session_t *)user);
}
