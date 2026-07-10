/**
 * @file v4l2_dmabuf.c
 * @brief V4L2 DMA-BUF 导出实现
 */
#include "v4l2_dmabuf.h"
#include "log.h"

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

int ocr_v4l2_export_dmabuf(int v4l2_fd, int index)
{
    struct v4l2_exportbuffer expbuf;
    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    expbuf.index = index;
    expbuf.flags = O_CLOEXEC;
    expbuf.fd = 0;

    if (ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf) < 0) {
        LOG_E("VIDIOC_EXPBUF[%d] 失败: %s", index, strerror(errno));
        return -1;
    }
    return expbuf.fd;
}
