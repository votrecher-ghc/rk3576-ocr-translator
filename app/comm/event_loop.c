/**
 * @file event_loop.c
 * @brief 事件循环实现（epoll 封装）
 */
#include "event_loop.h"
#include "log.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/epoll.h>

int ocr_event_loop_init(ocr_event_loop_t *loop, int max_events)
{
    if (!loop) return -1;
    if (max_events <= 0) max_events = 16;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        LOG_E("epoll_create1 失败: %s", strerror(errno));
        return -2;
    }
    loop->max_events = max_events;
    loop->running = 0;
    return 0;
}

void ocr_event_loop_destroy(ocr_event_loop_t *loop)
{
    if (!loop) return;
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
    }
}

int ocr_event_loop_add(ocr_event_loop_t *loop, const ocr_event_t *ev)
{
    if (!loop || !ev || ev->fd < 0) return -1;

    struct epoll_event ee;
    ee.events  = ev->events;
    /* epoll_data.ptr 指向事件项，回调时取回 */
    /* 注意：此处简化处理，将 user 与 cb 封装到 ocr_event_t 中需由调用方保活 */
    ee.data.ptr = (void *)ev;

    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, ev->fd, &ee) < 0) {
        LOG_E("epoll_ctl ADD fd=%d 失败: %s", ev->fd, strerror(errno));
        return -2;
    }
    return 0;
}

int ocr_event_loop_del(ocr_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0) return -1;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        LOG_W("epoll_ctl DEL fd=%d 失败: %s", fd, strerror(errno));
        return -2;
    }
    return 0;
}

int ocr_event_loop_run(ocr_event_loop_t *loop)
{
    if (!loop || loop->epoll_fd < 0) return -1;

    struct epoll_event *events = (struct epoll_event *)
        calloc(loop->max_events, sizeof(struct epoll_event));
    if (!events) {
        LOG_E("epoll 事件数组分配失败");
        return -2;
    }

    loop->running = 1;
    while (loop->running) {
        int n = epoll_wait(loop->epoll_fd, events, loop->max_events, 100);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_E("epoll_wait 失败: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < n; i++) {
            ocr_event_t *ev = (ocr_event_t *)events[i].data.ptr;
            if (ev && ev->cb) {
                ev->cb(ev->fd, events[i].events, ev->user);
            }
        }
    }

    free(events);
    return 0;
}

void ocr_event_loop_stop(ocr_event_loop_t *loop)
{
    if (!loop) return;
    loop->running = 0;
}
