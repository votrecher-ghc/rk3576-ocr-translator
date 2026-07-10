#ifndef OCR_INPUT_CAPTURE_SESSION_H
#define OCR_INPUT_CAPTURE_SESSION_H
/**
 * @file capture_session.h
 * @brief 拍照会话管理：按键→抓拍→JPEG编码→OCR→翻译→归档
 *
 * 监听拍照按键，触发单帧抓拍流程：编码 JPEG → OCR 识别 → 翻译 → 归档保存。
 */

#include "v4l2_capture.h"
#include "ocr_det.h"
#include "ocr_rec.h"
#include "translator.h"
#include "archiver.h"
#include "key_event.h"

/** 拍照会话状态 */
typedef enum {
    CAPTURE_SESSION_IDLE = 0,
    CAPTURE_SESSION_CAPTURING,  /* 抓拍中 */
    CAPTURE_SESSION_OCR,        /* OCR 识别中 */
    CAPTURE_SESSION_TRANSLATING,/* 翻译中 */
    CAPTURE_SESSION_SAVING,     /* 归档中 */
    CAPTURE_SESSION_DONE,
} capture_session_state_t;

/** 拍照会话上下文 */
typedef struct {
    ocr_v4l2_capture_t *capture; /* 采集器 */
    ocr_det_t          *det;     /* 文字检测 */
    ocr_rec_t          *rec;     /* 文字识别 */
    ocr_translator_t   *translator; /* 翻译器 */
    ocr_archiver_t     *archiver;/* 归档器 */
    capture_session_state_t state; /* 当前状态 */
    int                 busy;    /* 是否正在处理 */
} ocr_capture_session_t;

/**
 * @brief 初始化拍照会话
 */
int ocr_capture_session_init(ocr_capture_session_t *sess,
                             ocr_v4l2_capture_t *cap,
                             ocr_det_t *det, ocr_rec_t *rec,
                             ocr_translator_t *trs, ocr_archiver_t *arch);

/**
 * @brief 触发一次拍照（按键回调中调用）
 * @return 0=成功启动，1=忙碌忽略，负数=错误
 */
int ocr_capture_session_trigger(ocr_capture_session_t *sess);

/**
 * @brief 按键事件回调（注册到 key_event）
 */
void ocr_capture_session_on_key(ocr_key_event_t event, ocr_key_state_t state, void *user);

#endif /* OCR_INPUT_CAPTURE_SESSION_H */
