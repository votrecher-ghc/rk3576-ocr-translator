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

int ocr_msgbus_init(ocr_msgbus_t *bus, size_t capacity)
{
    if (!bus || capacity == 0) return -1;
    memset(bus, 0, sizeof(*bus));

    /* eventfd 用于唤醒消费者 */
    bus->eventfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (bus->eventfd < 0) {
        LOG_E("eventfd 创建失败: %s", strerror(errno));
        return -2;
    }

    /* 内部 SPSC 环形缓冲（多生产者需外部加锁，此处简化为单消费者） */
    /* TODO: 真正 MPSC 需用互斥锁保护 push，或改用无锁 MPSC 队列 */
    ocr_ringbuffer_t *ring = (ocr_ringbuffer_t *)malloc(sizeof(ocr_ringbuffer_t));
    if (!ring) {
        close(bus->eventfd);
        return -3;
    }
    if (ocr_ringbuffer_init(ring, sizeof(ocr_msg_t), capacity) != 0) {
        free(ring);
        close(bus->eventfd);
        return -4;
    }
    bus->ring = ring;
    bus->running = 1;
    return 0;
}

void ocr_msgbus_destroy(ocr_msgbus_t *bus)
{
    if (!bus) return;
    bus->running = 0;
    if (bus->ring) {
        ocr_ringbuffer_destroy((ocr_ringbuffer_t *)bus->ring);
        free(bus->ring);
        bus->ring = NULL;
    }
    if (bus->eventfd >= 0) {
        close(bus->eventfd);
        bus->eventfd = -1;
    }
}

int ocr_msgbus_post(ocr_msgbus_t *bus, const ocr_msg_t *msg)
{
    if (!bus || !msg) return -1;

    /* TODO: 多生产者场景下需在此加锁 */
    int ret = ocr_ringbuffer_push((ocr_ringbuffer_t *)bus->ring, msg);
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
    if (!bus || !msg) return -1;

    /* 先尝试直接读 */
    if (ocr_ringbuffer_pop((ocr_ringbuffer_t *)bus->ring, msg) == 0) {
        return 0;
    }

    if (timeout_ms == 0) {
        return 1; /* 非阻塞且空 */
    }

    /* poll eventfd */
    struct pollfd pfd;
    pfd.fd = bus->eventfd;
    pfd.events = POLLIN;

    int pret = poll(&pfd, 1, timeout_ms);
    if (pret <= 0) {
        return 1; /* 超时或错误 */
    }

    /* 清 eventfd 计数 */
    if (pfd.revents & POLLIN) {
        uint64_t val;
        read(bus->eventfd, &val, sizeof(val));
    }

    /* 再次尝试取消息 */
    if (ocr_ringbuffer_pop((ocr_ringbuffer_t *)bus->ring, msg) == 0) {
        return 0;
    }
    return 1;
}

int ocr_msgbus_get_fd(const ocr_msgbus_t *bus)
{
    return bus ? bus->eventfd : -1;
}
