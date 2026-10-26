/**
 * @file event_loop.c
 * @brief 线程安全的 epoll 事件循环封装
 *
 * epoll 事件循环：注册 fd 与回调，等待可读/可写事件后分发。
 * 内部 eventfd 用于唤醒阻塞的 epoll_wait，使另一线程发出的 stop 请求及时生效。
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
    ocr_event_t event; /**< 注册事件的值副本，内含借用的 fd 和 user；变更/分发快照由注册表锁保护。 */
    int active; /**< 当前注册仍有效为 1；del 置 0 后暂不 free，以容纳 epoll_wait 已返回的旧指针。 */
    struct ocr_event_item *next; /**< 所有分配过的注册项链表后继，含已失效项；循环销毁时统一遍历释放。 */
} ocr_event_item_t;

typedef struct {
    pthread_mutex_t lock; /**< 保护注册项链表、active 和 event 更新；业务回调在解锁后执行。 */
    ocr_event_item_t *items; /**< 本实现拥有的注册项链表首指针；add 分配，loop_destroy 才统一 free。 */
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

/** @brief 登记 fd 和回调并加入 epoll；拒绝重复注册同一个活动 fd。 */
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
    /* 内核事件返回 item 指针，避免收到事件时再按 fd 查表。
     * max_events 控制 epoll_wait 单批最多取多少条，不是允许注册的 fd 数上限。 */
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
    /* 已经被 epoll_wait 取到的事件仍可能保存 found 指针，所以这里只标记失效，
     * 不立即 free；对象延迟到整个 loop 销毁时释放，避免分发中访问已释放内存。 */
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

/** @brief 阻塞等待事件并执行回调；内部唤醒 fd 只用于控制循环，不作为业务事件。 */
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
            /* 拿锁复制回调后先解锁再执行，业务回调因此可以 stop 或修改事件注册。
             * del 只阻止尚未取得快照的分发，不等待已开始的回调；user 上下文的
             * 最终释放仍需上层保证回调已结束。 */
            if (snapshot.cb) snapshot.cb(snapshot.fd, events[i].events, snapshot.user);
        }
    }
    free(events);
    atomic_store(&loop->running, 0);
    return result;
}

/** @brief 清除运行标志并写 eventfd，唤醒正在 epoll_wait 中等待的线程。 */
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
