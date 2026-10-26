#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/** @file zerocopy.c @brief DMA-BUF descriptor transfer and cache sync.
 *
 * dup 或 SCM_RIGHTS 传递的是共享内存的引用，不复制像素，也不创建另一块图像内存。
 * DMA_BUF_IOCTL_SYNC 标记 CPU 访问区间并维护缓存一致性，不替代硬件完成等待和访问互斥。
 */
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

/** @brief 创建可独立关闭的新 fd；两个 fd 仍指向同一块 DMA-BUF，并非复制图像。 */
int ocr_zerocopy_dup_fd(int fd)
{
    int new_fd;

    if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
        return -1;
    }

    /* dup 只增加内核文件引用，不增加 ocr_buffer_t.refcount，也不阻止采集池
     * QBUF 后覆盖像素。跨模块保留“这帧内容”仍需应用层持有对应 buffer 引用。 */
    do {
        new_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    } while (new_fd < 0 && errno == EINTR);
    if (new_fd < 0) {
        LOG_E("duplicating fd=%d failed: %s", fd, strerror(errno));
        return -2;
    }
    return new_fd;
}

/** @brief 以 SCM_RIGHTS 发送内核文件引用；发送成功后发送方原 fd 仍有效。 */
int ocr_zerocopy_send_fd(int sock_fd, int fd)
{
    struct msghdr msg;
    struct iovec iov;
    /* control 是辅助数据区；CMSG_SPACE 包含头部及对齐填充，不能只分配 sizeof(int)。
     * union 的 cmsghdr 成员保证 bytes 起始地址满足控制消息的对齐要求。 */
    union {
        struct cmsghdr align; /**< 仅用于保证辅助数据区起始地址满足 cmsghdr 对齐，不存放要传递的 fd。 */
        unsigned char bytes[CMSG_SPACE(sizeof(int))]; /**< 含 CMSG 头及填充的单 fd 辅助数据区，栈上分配，仅本次 sendmsg/recvmsg 期间有效。 */
    } control;
    struct cmsghdr *cmsg;
    /* 用一个普通字节承载这次发送，fd 本身放辅助数据区，不是把整数当普通负载。
     * 接收方的 fd 数值由其进程描述符表分配，可以与发送方的 fd 完全不同。 */
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
    /* SCM_RIGHTS 告诉内核传递该 fd 引用的文件对象；send 成功不会关闭发送方 fd。
     * CMSG_LEN 是本条消息的有效长度，msg_controllen 仍用含填充的 CMSG_SPACE。 */
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

/** @brief 接收一个带 CLOEXEC 的 fd，拒绝截断或多余描述符，并关闭意外收到的 fd。 */
int ocr_zerocopy_recv_fd(int sock_fd)
{
    struct msghdr msg;
    struct iovec iov;
    union {
        struct cmsghdr align; /**< 仅用于保证辅助数据区起始地址满足 cmsghdr 对齐，不存放要传递的 fd。 */
        unsigned char bytes[CMSG_SPACE(sizeof(int))]; /**< 含 CMSG 头及填充的单 fd 辅助数据区，栈上分配，仅本次 sendmsg/recvmsg 期间有效。 */
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

    /* 原子设置接收 fd 的 CLOEXEC，避免另一个线程恰好 exec 时继承新 fd。
     * 后面的 fcntl 再做校验/补设，失败路径必须关闭已经安装到本进程的 fd。 */
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
                /* 协议只接受一份 fd；多余 fd 也已经被内核安装，不能只报错不 close，
                 * 否则畸形消息会逐次消耗进程的描述符资源。 */
                (void)close(fd);
                malformed = 1;
            }
        }
    }

    /* 控制区被截断时无法证明完整接收了预期协议，关闭已接收项后拒绝返回。
     * 此接口只传文件引用，不附带宽高/stride/frame_id 或跨进程的回收通知。 */
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

/** @brief 将 CPU 读/写开始与结束映射到 DMA_BUF_IOCTL_SYNC；硬件完成等待由调用方负责。 */
int ocr_zerocopy_sync(int fd, int sync)
{
    /* READ/WRITE 均相对 CPU；START/END 应与同一次 CPU 访问配对。
     * 这张表没有设备完成 fence 的含义，也不会锁住其他线程或硬件。 */
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
    /* CPU 读硬件写入结果：先确认硬件完成 -> START|READ -> 访问 mmap -> END|READ。
     * CPU 写后交硬件：START|WRITE -> 写 mmap -> END|WRITE -> 提交硬件任务。
     * 调用时序由上层保证，此函数不记录 START/END 是否配对，也不更新引用计数。 */
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
