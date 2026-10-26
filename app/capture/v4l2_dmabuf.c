/**
 * @file v4l2_dmabuf.c
 * @brief V4L2 DMA-BUF 导出实现
 *
 * 为已有 V4L2 MMAP 缓冲导出共享 fd；导出操作不分配另一幅图像，也不等待采集完成。
 * 使用方仍须遵守 DQBUF/QBUF 时序；成功返回的 fd 需要由调用方或接管它的缓冲池关闭。
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
    /* (type,index,plane) 精确定位已经 QUERYBUF 的那块驱动内存。
     * type 必须与 REQBUFS 的队列类型一致；plane 是内存平面索引，不是 NV12 的 UV。 */
    expbuf.type = buffer_type;
    expbuf.index = index;
    expbuf.plane = plane;
    expbuf.flags = O_CLOEXEC;
    /* fd 是 ioctl 的输出字段，此处的 0 不是要导出到标准输入；成功后由内核填入。
     * 返回 fd=0 仍合法，所以后面只用 <0 检查错误。 */
    expbuf.fd = 0;

    /* EXPBUF 为已经分配的 V4L2 缓冲建立 DMA-BUF fd，不搬运像素。
     * fd 交给缓冲池管理，初始化失败时由调用者关闭。 */
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
