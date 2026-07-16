/**
 * @file msgbus.c
 * @brief 消息总线实现（无锁环形缓冲 + eventfd）
 */
#include "msgbus.h"
#include "ringbuffer.h"
#include "log.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <pthread.h>

typedef struct {
    ocr_ringbuffer_t ring;
    pthread_mutex_t  producer_lock;
} ocr_msgbus_priv_t;

static int msgbus_pop(ocr_msgbus_t *bus, ocr_msg_t *msg)
{
    ocr_msgbus_priv_t *priv = (ocr_msgbus_priv_t *)bus->ring;
    return priv ? ocr_ringbuffer_pop(&priv->ring, msg) : -1;
}

static void msgbus_consume_signal(ocr_msgbus_t *bus)
{
    uint64_t value;
    ssize_t n;
    do {
        n = read(bus->eventfd, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
}

int ocr_msgbus_init(ocr_msgbus_t *bus, size_t capacity)
{
    if (!bus || capacity == 0) return -1;
    memset(bus, 0, sizeof(*bus));
    bus->eventfd = -1;
    bus->epoll_fd = -1;

    /* eventfd 用于唤醒消费者 */
    bus->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (bus->eventfd < 0) {
        LOG_E("eventfd 创建失败: %s", strerror(errno));
        return -2;
    }

    ocr_msgbus_priv_t *priv = (ocr_msgbus_priv_t *)calloc(1, sizeof(*priv));
    if (!priv) {
        close(bus->eventfd);
        bus->eventfd = -1;
        return -3;
    }
    if (pthread_mutex_init(&priv->producer_lock, NULL) != 0) {
        free(priv);
        close(bus->eventfd);
        bus->eventfd = -1;
        return -4;
    }
    if (ocr_ringbuffer_init(&priv->ring, sizeof(ocr_msg_t), capacity) != 0) {
        pthread_mutex_destroy(&priv->producer_lock);
        free(priv);
        close(bus->eventfd);
        bus->eventfd = -1;
        return -5;
    }
    bus->ring = priv;
    bus->running = 1;
    return 0;
}

void ocr_msgbus_destroy(ocr_msgbus_t *bus)
{
    if (!bus) return;
    bus->running = 0;
    if (bus->ring) {
        ocr_msgbus_priv_t *priv = (ocr_msgbus_priv_t *)bus->ring;
        ocr_ringbuffer_destroy(&priv->ring);
        pthread_mutex_destroy(&priv->producer_lock);
        free(priv);
        bus->ring = NULL;
    }
    if (bus->eventfd >= 0) {
        close(bus->eventfd);
        bus->eventfd = -1;
    }
}

int ocr_msgbus_post(ocr_msgbus_t *bus, const ocr_msg_t *msg)
{
    if (!bus || !msg || !bus->running || !bus->ring || bus->eventfd < 0) return -1;

    ocr_msgbus_priv_t *priv = (ocr_msgbus_priv_t *)bus->ring;
    pthread_mutex_lock(&priv->producer_lock);
    int ret = ocr_ringbuffer_push(&priv->ring, msg);
    pthread_mutex_unlock(&priv->producer_lock);
    if (ret != 0) {
        return ret;
    }

    /* 写 eventfd 唤醒消费者 */
    uint64_t val = 1;
    if (write(bus->eventfd, &val, sizeof(val)) < 0 && errno != EAGAIN) {
        LOG_W("eventfd write 失败: %s", strerror(errno));
    }
    return 0;
}

int ocr_msgbus_recv(ocr_msgbus_t *bus, ocr_msg_t *msg, int timeout_ms)
{
    if (!bus || !msg || !bus->running || !bus->ring || bus->eventfd < 0) return -1;

    /* 先尝试直接读 */
    if (msgbus_pop(bus, msg) == 0) {
        msgbus_consume_signal(bus);
        return 0;
    }

    if (timeout_ms == 0) {
        return 1; /* 非阻塞且空 */
    }

    /* poll eventfd */
    struct pollfd pfd;
    pfd.fd = bus->eventfd;
    pfd.events = POLLIN;

    int pret;
    do {
        pret = poll(&pfd, 1, timeout_ms);
    } while (pret < 0 && errno == EINTR);
    if (pret <= 0) {
        return pret == 0 ? 1 : -2;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -3;

    /* 清 eventfd 计数 */
    if (pfd.revents & POLLIN) {
        msgbus_consume_signal(bus);
    }

    /* 再次尝试取消息 */
    if (msgbus_pop(bus, msg) == 0) {
        return 0;
    }
    return 1;
}

int ocr_msgbus_get_fd(const ocr_msgbus_t *bus)
{
    return bus ? bus->eventfd : -1;
}
