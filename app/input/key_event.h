#ifndef OCR_INPUT_KEY_EVENT_H
#define OCR_INPUT_KEY_EVENT_H
/**
 * @file key_event.h
 * @brief Input 子系统事件读取（/dev/input/eventX, KEY_CAMERA 等）
 *
 * 发现支持 KEY_CAMERA 的 input 设备，在监听线程中读取按下/释放事件并调用业务回调。
 * 自动发现只接受唯一匹配设备；停止时回收线程后再关闭 fd，回调在监听线程中同步执行。
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
    int          dev_fd; /**< 本监听器拥有的 input event fd，init 打开、线程停止后 destroy 关闭。 */
    char         dev_path[64]; /**< 复制的 /dev/input/eventN 路径，含 NUL；设备编号可由自动发现获得。 */
    atomic_int   running; /**< 原子运行请求/状态；stop 或读取异常置 0，不代表线程句柄已 join。 */
    atomic_int   thread_started; /**< 线程创建后且尚未 join 为 1；自行退出仍须由管理线程回收。 */
    atomic_int   worker_error; /**< 原子记录的线程退出原因，0 正常、负 errno 异常，供主循环检测。 */
    int          initialized; /**< 设备及回调配置建立成功标志；destroy 安全完成后清零。 */
    ocr_thread_t thread; /**< 本监听器的工作线程句柄；on_key 就在该线程同步执行。 */
    /* 事件回调 */
    void       (*on_key)(ocr_key_event_t event, ocr_key_state_t state, void *user); /**< 借用的事件处理函数，传入内部键类型/按下释放状态；耗时回调会阻塞继续读键。 */
    void        *user_ctx; /**< 借给 on_key 的业务上下文，如拍照会话；须保持到监听线程 join。 */
} ocr_key_event_t_ctx;

/** Discover an input event device that advertises KEY_CAMERA.
 *
 * 遍历 event 节点并要求唯一 KEY_CAMERA 设备；多个匹配时交由配置显式选择。
 */
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
