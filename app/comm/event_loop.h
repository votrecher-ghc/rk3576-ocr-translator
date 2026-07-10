#ifndef OCR_COMM_EVENT_LOOP_H
#define OCR_COMM_EVENT_LOOP_H
/**
 * @file event_loop.h
 * @brief 事件循环：epoll 封装
 *
 * 单线程事件循环，监听多个 fd（eventfd/input/iio 等），
 * 事件就绪时调用注册的回调。
 */

#include <stdint.h>
#include <stddef.h>

/** 事件回调函数原型
 * @param fd     就绪的文件描述符
 * @param events epoll 事件掩码
 * @param user   用户自定义上下文
 */
typedef void (*ocr_event_cb_t)(int fd, uint32_t events, void *user);

/** 事件项 */
typedef struct {
    int            fd;       /* 监听的 fd */
    uint32_t       events;   /* 关注的事件掩码（EPOLLIN/EPOLLET 等） */
    ocr_event_cb_t cb;       /* 回调函数 */
    void          *user;     /* 用户上下文 */
} ocr_event_t;

/** 事件循环上下文 */
typedef struct {
    int          epoll_fd;   /* epoll 实例 fd */
    int          running;    /* 运行标志 */
    int          max_events; /* 最大事件数 */
} ocr_event_loop_t;

/**
 * @brief 初始化事件循环
 * @param[in] max_events 单次 epoll_wait 最大返回事件数
 * @return 0=成功，负数=错误
 */
int ocr_event_loop_init(ocr_event_loop_t *loop, int max_events);

/**
 * @brief 销毁事件循环
 */
void ocr_event_loop_destroy(ocr_event_loop_t *loop);

/**
 * @brief 添加 fd 监听
 * @param[in] loop   事件循环
 * @param[in] ev     事件描述（fd/events/cb/user）
 * @return 0=成功，负数=错误
 */
int ocr_event_loop_add(ocr_event_loop_t *loop, const ocr_event_t *ev);

/**
 * @brief 删除 fd 监听
 */
int ocr_event_loop_del(ocr_event_loop_t *loop, int fd);

/**
 * @brief 运行事件循环（阻塞直到 ocr_event_loop_stop 被调用）
 */
int ocr_event_loop_run(ocr_event_loop_t *loop);

/**
 * @brief 停止事件循环
 */
void ocr_event_loop_stop(ocr_event_loop_t *loop);

#endif /* OCR_COMM_EVENT_LOOP_H */
