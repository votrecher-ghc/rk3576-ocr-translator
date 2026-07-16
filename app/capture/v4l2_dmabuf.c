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
    return ocr_v4l2_export_dmabuf_ex(v4l2_fd,
                                     V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                     index, 0);
}

int ocr_v4l2_export_dmabuf_ex(int v4l2_fd, uint32_t buffer_type,
                              int index, uint32_t plane)
{
    if (v4l2_fd < 0 || index < 0 ||
        (buffer_type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
         buffer_type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
        (buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE && plane != 0)) {
        return -1;
    }
    struct v4l2_exportbuffer expbuf;
    memset(&expbuf, 0, sizeof(expbuf));
    expbuf.type = buffer_type;
    expbuf.index = index;
    expbuf.plane = plane;
    expbuf.flags = O_CLOEXEC;
    expbuf.fd = 0;

    int ret;
    do {
        ret = ioctl(v4l2_fd, VIDIOC_EXPBUF, &expbuf);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        LOG_E("VIDIOC_EXPBUF[%d] 失败: %s", index, strerror(errno));
        return -1;
    }
    if (expbuf.fd < 0) {
        LOG_E("VIDIOC_EXPBUF[%d] returned invalid fd", index);
        return -1;
    }
    return expbuf.fd;
}
