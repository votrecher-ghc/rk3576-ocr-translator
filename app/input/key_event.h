#ifndef OCR_INPUT_KEY_EVENT_H
#define OCR_INPUT_KEY_EVENT_H
/**
 * @file key_event.h
 * @brief Input 子系统事件读取（/dev/input/eventX, KEY_CAMERA 等）
 */

#include <stdint.h>
#include <stdatomic.h>
#include "thread.h"

/** 按键事件类型 */
typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_CAMERA,    /* 拍照键 */
    KEY_EVENT_MODE,      /* 模式切换键 */
    KEY_EVENT_POWER,     /* 电源键 */
} ocr_key_event_t;

/** 按键状态 */
typedef enum {
    KEY_STATE_RELEASED = 0,
    KEY_STATE_PRESSED,
} ocr_key_state_t;

/** 按键事件上下文 */
typedef struct {
    int          dev_fd;     /* /dev/input/eventX fd */
    char         dev_path[64];
    atomic_int   running;    /* 监听线程运行标志 */
    ocr_thread_t thread;
    /* 事件回调 */
    void       (*on_key)(ocr_key_event_t event, ocr_key_state_t state, void *user);
    void        *user_ctx;
} ocr_key_event_t_ctx;

/**
 * @brief 初始化按键事件监听
 * @param[in] ke       按键上下文
 * @param[in] dev_path input 设备路径（如 /dev/input/event0）
 * @param[in] cb       按键回调
 * @param[in] user     用户上下文
 * @return 0=成功，负数=错误
 */
int ocr_key_event_init(ocr_key_event_t_ctx *ke, const char *dev_path,
                       void (*cb)(ocr_key_event_t, ocr_key_state_t, void *),
                       void *user);

/**
 * @brief 启动按键监听线程
 */
int ocr_key_event_start(ocr_key_event_t_ctx *ke);

/**
 * @brief 停止按键监听
 */
int ocr_key_event_stop(ocr_key_event_t_ctx *ke);

/**
 * @brief 销毁按键监听
 */
void ocr_key_event_destroy(ocr_key_event_t_ctx *ke);

#endif /* OCR_INPUT_KEY_EVENT_H */
