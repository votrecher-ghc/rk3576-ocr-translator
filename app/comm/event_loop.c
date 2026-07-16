/**
 * @file event_loop.c
 * @brief 线程安全的 epoll 事件循环封装
 */
#include "event_loop.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

typedef struct ocr_event_item {
    ocr_event_t event;
    int active;
    struct ocr_event_item *next;
} ocr_event_item_t;

typedef struct {
    pthread_mutex_t lock;
    ocr_event_item_t *items;
} ocr_event_loop_impl_t;

static void drain_wake_fd(int fd)
{
    uint64_t value;
    while (read(fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
        /* eventfd 为非阻塞；读到 EAGAIN 即已清空。 */
    }
}

int ocr_event_loop_init(ocr_event_loop_t *loop, int max_events)
{
    if (!loop) return -1;
    if (max_events <= 0) max_events = 16;

    memset(loop, 0, sizeof(*loop));
    loop->epoll_fd = -1;
    loop->wake_fd = -1;
    loop->max_events = max_events;
    atomic_init(&loop->running, 0);

    ocr_event_loop_impl_t *impl = calloc(1, sizeof(*impl));
    if (!impl || pthread_mutex_init(&impl->lock, NULL) != 0) {
        free(impl);
        return -2;
    }
    loop->impl = impl;

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        LOG_E("epoll_create1 失败: %s", strerror(errno));
        ocr_event_loop_destroy(loop);
        return -3;
    }
    loop->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (loop->wake_fd < 0) {
        LOG_E("eventfd 创建失败: %s", strerror(errno));
        ocr_event_loop_destroy(loop);
        return -4;
    }

    struct epoll_event wake = {0};
    wake.events = EPOLLIN;
    wake.data.ptr = NULL;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, loop->wake_fd, &wake) != 0) {
        LOG_E("注册事件循环唤醒 fd 失败: %s", strerror(errno));
        ocr_event_loop_destroy(loop);
        return -5;
    }
    return 0;
}

void ocr_event_loop_destroy(ocr_event_loop_t *loop)
{
    if (!loop) return;
    ocr_event_loop_stop(loop);
    if (loop->wake_fd >= 0) close(loop->wake_fd);
    if (loop->epoll_fd >= 0) close(loop->epoll_fd);
    loop->wake_fd = -1;
    loop->epoll_fd = -1;

    ocr_event_loop_impl_t *impl = loop->impl;
    if (impl) {
        ocr_event_item_t *item = impl->items;
        while (item) {
            ocr_event_item_t *next = item->next;
            free(item);
            item = next;
        }
        pthread_mutex_destroy(&impl->lock);
        free(impl);
        loop->impl = NULL;
    }
}

int ocr_event_loop_add(ocr_event_loop_t *loop, const ocr_event_t *ev)
{
    if (!loop || !ev || ev->fd < 0 || !ev->cb || loop->epoll_fd < 0 || !loop->impl)
        return -1;

    ocr_event_loop_impl_t *impl = loop->impl;
    ocr_event_item_t *item = calloc(1, sizeof(*item));
    if (!item) return -2;
    item->event = *ev;
    item->active = 1;

    pthread_mutex_lock(&impl->lock);
    for (ocr_event_item_t *it = impl->items; it; it = it->next) {
        if (it->active && it->event.fd == ev->fd) {
            pthread_mutex_unlock(&impl->lock);
            free(item);
            return -3;
        }
    }

    struct epoll_event ee = {0};
    ee.events = ev->events;
    ee.data.ptr = item;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, ev->fd, &ee) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&impl->lock);
        free(item);
        LOG_E("epoll_ctl ADD fd=%d 失败: %s", ev->fd, strerror(saved_errno));
        return -4;
    }
    item->next = impl->items;
    impl->items = item;
    pthread_mutex_unlock(&impl->lock);
    return 0;
}

int ocr_event_loop_del(ocr_event_loop_t *loop, int fd)
{
    if (!loop || fd < 0 || loop->epoll_fd < 0 || !loop->impl) return -1;
    ocr_event_loop_impl_t *impl = loop->impl;
    pthread_mutex_lock(&impl->lock);
    ocr_event_item_t *found = NULL;
    for (ocr_event_item_t *it = impl->items; it; it = it->next) {
        if (it->active && it->event.fd == fd) {
            found = it;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&impl->lock);
        return -2;
    }
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) != 0 && errno != ENOENT) {
        int saved_errno = errno;
        pthread_mutex_unlock(&impl->lock);
        LOG_W("epoll_ctl DEL fd=%d 失败: %s", fd, strerror(saved_errno));
        return -3;
    }
    found->active = 0;
    pthread_mutex_unlock(&impl->lock);
    return 0;
}

int ocr_event_loop_mod(ocr_event_loop_t *loop, const ocr_event_t *ev)
{
    if (!loop || !ev || ev->fd < 0 || !ev->cb || loop->epoll_fd < 0 || !loop->impl)
        return -1;
    ocr_event_loop_impl_t *impl = loop->impl;
    pthread_mutex_lock(&impl->lock);
    ocr_event_item_t *found = NULL;
    for (ocr_event_item_t *it = impl->items; it; it = it->next) {
        if (it->active && it->event.fd == ev->fd) {
            found = it;
            break;
        }
    }
    if (!found) {
        pthread_mutex_unlock(&impl->lock);
        return -2;
    }
    struct epoll_event ee = {0};
    ee.events = ev->events;
    ee.data.ptr = found;
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, ev->fd, &ee) != 0) {
        int saved_errno = errno;
        pthread_mutex_unlock(&impl->lock);
        LOG_E("epoll_ctl MOD fd=%d 失败: %s", ev->fd, strerror(saved_errno));
        return -3;
    }
    found->event = *ev;
    pthread_mutex_unlock(&impl->lock);
    return 0;
}

int ocr_event_loop_run(ocr_event_loop_t *loop)
{
    if (!loop || loop->epoll_fd < 0 || loop->wake_fd < 0 || !loop->impl) return -1;
    if (atomic_exchange(&loop->running, 1)) return -2;
    drain_wake_fd(loop->wake_fd);

    struct epoll_event *events = calloc((size_t)loop->max_events, sizeof(*events));
    if (!events) {
        atomic_store(&loop->running, 0);
        return -3;
    }

    int result = 0;
    while (atomic_load(&loop->running)) {
        int n = epoll_wait(loop->epoll_fd, events, loop->max_events, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_E("epoll_wait 失败: %s", strerror(errno));
            result = -4;
            break;
        }
        for (int i = 0; i < n; ++i) {
            ocr_event_item_t *item = events[i].data.ptr;
            if (!item) {
                drain_wake_fd(loop->wake_fd);
                continue;
            }

            ocr_event_t snapshot = {0};
            ocr_event_loop_impl_t *impl = loop->impl;
            pthread_mutex_lock(&impl->lock);
            if (item->active) snapshot = item->event;
            pthread_mutex_unlock(&impl->lock);
            if (snapshot.cb) snapshot.cb(snapshot.fd, events[i].events, snapshot.user);
        }
    }
    free(events);
    atomic_store(&loop->running, 0);
    return result;
}

void ocr_event_loop_stop(ocr_event_loop_t *loop)
{
    if (!loop) return;
    atomic_store(&loop->running, 0);
    if (loop->wake_fd >= 0) {
        uint64_t value = 1;
        ssize_t n;
        do {
            n = write(loop->wake_fd, &value, sizeof(value));
        } while (n < 0 && errno == EINTR);
    }
}
