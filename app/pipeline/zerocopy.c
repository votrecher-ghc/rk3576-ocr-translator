#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/** @file zerocopy.c @brief DMA-BUF descriptor transfer and cache sync. */
#include "zerocopy.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

int ocr_zerocopy_dup_fd(int fd)
{
    int new_fd;

    if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    do {
        new_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (new_fd < 0 && errno == EINTR);
    if (new_fd < 0) {
        LOG_E("duplicating fd=%d failed: %s", fd, strerror(errno));
        return -2;
    }
    return new_fd;
}

int ocr_zerocopy_send_fd(int sock_fd, int fd)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct cmsghdr *cmsg;
    char payload = 'F';
    ssize_t sent;

    if (sock_fd < 0 || fd < 0 ||
        fcntl(sock_fd, F_GETFD) < 0 || fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &payload;
    iov.iov_len = sizeof(payload);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);

    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg) {
        return -2;
    }
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    do {
        sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)sizeof(payload)) {
        if (sent < 0) {
            LOG_E("sending fd failed: %s", strerror(errno));
        }
        return -2;
    }
    return 0;
}

int ocr_zerocopy_recv_fd(int sock_fd)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct cmsghdr *cmsg;
    char payload = 0;
    ssize_t received;
    int received_fd = -1;
    int malformed = 0;

    if (sock_fd < 0 || fcntl(sock_fd, F_GETFD) < 0) {
        return -1;
    }

    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    iov.iov_base = &payload;
    iov.iov_len = sizeof(payload);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);

    do {
        received = recvmsg(sock_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        LOG_E("receiving fd failed: %s", strerror(errno));
        return -2;
    }
    if (received == 0) {
        return -2;
    }

    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        size_t payload_size;
        size_t fd_count;

        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            malformed = 1;
            break;
        }
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }

        payload_size = cmsg->cmsg_len - CMSG_LEN(0);
        if (payload_size == 0 || payload_size % sizeof(int) != 0) {
            malformed = 1;
            break;
        }
        fd_count = payload_size / sizeof(int);
        for (size_t i = 0; i < fd_count; ++i) {
            int fd;
            memcpy(&fd, (unsigned char *)CMSG_DATA(cmsg) + i * sizeof(int),
                   sizeof(fd));
            if (received_fd < 0) {
                received_fd = fd;
            } else {
                (void)close(fd);
                malformed = 1;
            }
        }
    }

    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        malformed = 1;
    }
    if (malformed || received_fd < 0) {
        if (received_fd >= 0) {
            (void)close(received_fd);
        }
        return -3;
    }

    int flags = fcntl(received_fd, F_GETFD);
    if (flags < 0 ||
        (!(flags & FD_CLOEXEC) &&
         fcntl(received_fd, F_SETFD, flags | FD_CLOEXEC) < 0)) {
        (void)close(received_fd);
        return -2;
    }
    return received_fd;
}

int ocr_zerocopy_sync(int fd, int sync)
{
    static const uint64_t flags_map[4] = {
        DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END,
        DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_WRITE | DMA_BUF_SYNC_END,
    };
    struct dma_buf_sync sync_arg;
    int ret;

    if (fd < 0 || sync < 0 || sync >= (int)(sizeof(flags_map) /
                                             sizeof(flags_map[0])) ||
        fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    memset(&sync_arg, 0, sizeof(sync_arg));
    sync_arg.flags = flags_map[sync];
    do {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_arg);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        LOG_W("DMA_BUF_IOCTL_SYNC failed for fd=%d: %s",
              fd, strerror(errno));
        return -2;
    }
    return 0;
}
