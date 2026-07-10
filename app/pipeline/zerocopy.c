/**
 * @file zerocopy.c
 * @brief DMA-BUF 零拷贝传递实现
 */
#include "zerocopy.h"
#include "log.h"

#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <linux/dma-buf.h>

int ocr_zerocopy_dup_fd(int fd)
{
    if (fd < 0) return -1;
    int new_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (new_fd < 0) {
        LOG_E("dup fd=%d 失败: %s", fd, strerror(errno));
        return -2;
    }
    return new_fd;
}

int ocr_zerocopy_send_fd(int sock_fd, int fd)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    memset(buf, 0, sizeof(buf));

    /* 至少发送一个字节，否则 recvmsg 可能不返回辅助数据 */
    char dummy = 'x';
    iov.iov_base = &dummy;
    iov.iov_len  = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if (sendmsg(sock_fd, &msg, 0) < 0) {
        LOG_E("sendmsg 发送 fd 失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int ocr_zerocopy_recv_fd(int sock_fd)
{
    struct msghdr msg = {0};
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int))];
    char dummy;

    iov.iov_base = &dummy;
    iov.iov_len  = 1;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg(sock_fd, &msg, 0) < 0) {
        LOG_E("recvmsg 接收 fd 失败: %s", strerror(errno));
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    while (cmsg) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd;
            memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
            return fd;
        }
        cmsg = CMSG_NXTHDR(&msg, cmsg);
    }
    return -2;
}

int ocr_zerocopy_sync(int fd, int sync)
{
    /* DMA_BUF_IOCTL_SYNC 同步缓存 */
    struct dma_buf_sync sync_arg;
    memset(&sync_arg, 0, sizeof(sync_arg));

    static const uint64_t flags_map[4] = {
        DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END,
        DMA_BUF_SYNC_RW   | DMA_BUF_SYNC_START,
        DMA_BUF_SYNC_RW   | DMA_BUF_SYNC_END,
    };
    if (sync < 0 || sync > 3) return -1;
    sync_arg.flags = flags_map[sync];

    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_arg) < 0) {
        LOG_W("DMA_BUF_IOCTL_SYNC 失败 fd=%d: %s", fd, strerror(errno));
        return -2;
    }
    return 0;
}
