/**
 * @file capture_session.c
 * @brief 拍照会话管理实现
 */
#include "capture_session.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <jpeglib.h>

/* 将一帧图像编码为 JPEG */
static int encode_jpeg(const ocr_buffer_t *buf, const char *out_path)
{
    if (!buf || !out_path) return -1;

    /* TODO: 若 buf 为 NV12/YUYV，需先转 RGB888（可用 RGA） */
    /* 当前假设 buf->mmap_addr 已是 RGB888 */

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *fp = NULL;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    fp = fopen(out_path, "wb");
    if (!fp) {
        LOG_E("创建 JPEG 文件失败: %s", out_path);
        jpeg_destroy_compress(&cinfo);
        return -2;
    }
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = buf->width;
    cinfo.image_height = buf->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 90, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    /* 逐行写入 */
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)((uint8_t *)buf->mmap_addr +
                                  cinfo.next_scanline * buf->width * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);
    return 0;
}

int ocr_capture_session_init(ocr_capture_session_t *sess,
                             ocr_v4l2_capture_t *cap,
                             ocr_det_t *det, ocr_rec_t *rec,
                             ocr_translator_t *trs, ocr_archiver_t *arch)
{
    if (!sess) return -1;
    memset(sess, 0, sizeof(*sess));
    sess->capture = cap;
    sess->det = det;
    sess->rec = rec;
    sess->translator = trs;
    sess->archiver = arch;
    sess->state = CAPTURE_SESSION_IDLE;
    sess->busy = 0;
    return 0;
}

int ocr_capture_session_trigger(ocr_capture_session_t *sess)
{
    if (!sess) return -1;
    if (sess->busy) {
        LOG_W("拍照会话忙碌，忽略触发");
        return 1;
    }
    sess->busy = 1;
    sess->state = CAPTURE_SESSION_CAPTURING;
    LOG_I("==== 拍照会话启动 ====");

    /* TODO: 从采集器获取当前帧（需暂停管线或捕获最新帧） */
    /* 1. 抓拍一帧 */
    ocr_buffer_t *frame = NULL;
    /* frame = ocr_v4l2_grab_frame(sess->capture); */
    if (!frame) {
        LOG_E("抓拍失败");
        sess->busy = 0;
        sess->state = CAPTURE_SESSION_IDLE;
        return -2;
    }

    /* 2. JPEG 编码 */
    sess->state = CAPTURE_SESSION_SAVING;
    char img_path[512];
    /* TODO: 由 archiver 生成本次归档目录 */
    snprintf(img_path, sizeof(img_path), "/tmp/capture_%llu.jpg",
             (unsigned long long)frame->frame_id);
    if (encode_jpeg(frame, img_path) != 0) {
        LOG_E("JPEG 编码失败");
        sess->busy = 0;
        sess->state = CAPTURE_SESSION_IDLE;
        return -3;
    }

    /* 3. OCR 检测 */
    sess->state = CAPTURE_SESSION_OCR;
    ocr_text_box_list_t boxes;
    if (ocr_det_run(sess->det, frame, &boxes) == 0 && boxes.count > 0) {
        /* 4. OCR 识别 */
        char texts[OCR_MAX_TEXT_BOXES][OCR_MAX_TEXT_LEN];
        char *text_ptrs[OCR_MAX_TEXT_BOXES];
        for (int i = 0; i < boxes.count; i++) text_ptrs[i] = texts[i];
        ocr_rec_run_batch(sess->rec, frame, &boxes, text_ptrs);

        /* 5. 翻译 */
        sess->state = CAPTURE_SESSION_TRANSLATING;
        for (int i = 0; i < boxes.count; i++) {
            char translated[OCR_MAX_TEXT_LEN];
            if (translator_translate(sess->translator, texts[i],
                                     translated, sizeof(translated)) == 0) {
                LOG_I("[%d] '%s' → '%s'", i, texts[i], translated);
            }
        }
    }

    /* 6. 归档 */
    sess->state = CAPTURE_SESSION_SAVING;
    if (sess->archiver) {
        /* TODO: 调用 archiver 保存 image.jpg + result.json + result.txt */
    }

    sess->state = CAPTURE_SESSION_DONE;
    sess->busy = 0;
    LOG_I("==== 拍照会话完成 ====");
    return 0;
}

void ocr_capture_session_on_key(ocr_key_event_t event, ocr_key_state_t state, void *user)
{
    ocr_capture_session_t *sess = (ocr_capture_session_t *)user;
    if (event == KEY_EVENT_CAMERA && state == KEY_STATE_PRESSED) {
        ocr_capture_session_trigger(sess);
    }
}
