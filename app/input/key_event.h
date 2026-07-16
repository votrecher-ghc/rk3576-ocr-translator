#ifndef OCR_INPUT_KEY_EVENT_H
#define OCR_INPUT_KEY_EVENT_H
/**
 * @file key_event.h
 * @brief Input 子系统事件读取（/dev/input/eventX, KEY_CAMERA 等）
 */

#include <stdint.h>
#include <stddef.h>
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
    atomic_int   running;        /* worker 运行请求/当前运行状态 */
    atomic_int   thread_started; /* thread handle 已创建且尚未 join */
    atomic_int   worker_error;   /* worker 异常退出原因（负 errno） */
    int          initialized;
    ocr_thread_t thread;
    /* 事件回调 */
    void       (*on_key)(ocr_key_event_t event, ocr_key_state_t state, void *user);
    void        *user_ctx;
} ocr_key_event_t_ctx;

/** Discover an input event device that advertises KEY_CAMERA. */
int ocr_key_event_find_device(char *dev_path, size_t dev_path_size);

/**
 * @brief 初始化按键事件监听
 * @param[in] ke       按键上下文
 * @param[in] dev_path 动态发现的 input 设备路径（如 /dev/input/eventN）
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

/** Return zero while healthy, or the negative worker error after an exit. */
int ocr_key_event_get_worker_error(const ocr_key_event_t_ctx *ke);

/** Return 1 only while the input worker is actively running. */
int ocr_key_event_is_running(const ocr_key_event_t_ctx *ke);

/**
 * @brief 销毁按键监听
 */
void ocr_key_event_destroy(ocr_key_event_t_ctx *ke);

#endif /* OCR_INPUT_KEY_EVENT_H */
