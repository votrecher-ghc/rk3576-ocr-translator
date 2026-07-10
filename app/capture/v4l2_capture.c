/**
 * @file v4l2_capture.c
 * @brief V4L2 采集封装实现
 */
#include "v4l2_capture.h"
#include "v4l2_dmabuf.h"
#include "log.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

/* ocr 像素格式 → V4L2 fourcc */
static uint32_t fmt_to_fourcc(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:     return V4L2_PIX_FMT_NV12;
        case OCR_FMT_NV16:     return V4L2_PIX_FMT_NV16;
        case OCR_FMT_YUYV:     return V4L2_PIX_FMT_YUYV;
        case OCR_FMT_RGB565:   return V4L2_PIX_FMT_RGB565;
        case OCR_FMT_RGB888:   return V4L2_PIX_FMT_RGB24;
        case OCR_FMT_ARGB8888: return V4L2_PIX_FMT_ARGB32;
        case OCR_FMT_BGRA8888: return V4L2_PIX_FMT_ABGR32;
        default:               return 0;
    }
}

/* xioctl: 重试被中断的 ioctl */
static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

int ocr_v4l2_open(ocr_v4l2_capture_t *cap, const char *dev_name,
                  uint32_t width, uint32_t height, uint32_t fps,
                  ocr_pixel_format_t format)
{
    if (!cap || !dev_name) return -1;
    memset(cap, 0, sizeof(*cap));

    cap->fd = open(dev_name, O_RDWR | O_CLOEXEC);
    if (cap->fd < 0) {
        LOG_E("打开 V4L2 设备 %s 失败: %s", dev_name, strerror(errno));
        return -2;
    }
    strncpy(cap->dev_name, dev_name, sizeof(cap->dev_name) - 1);
    cap->width = width;
    cap->height = height;
    cap->fps = fps;
    cap->format = format;
    cap->buf_count = V4L2_MAX_BUFS;

    /* 查询能力 */
    struct v4l2_capability caps;
    memset(&caps, 0, sizeof(caps));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &caps) < 0) {
        LOG_E("VIDIOC_QUERYCAP 失败: %s", strerror(errno));
        goto err;
    }
    if (!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_E("设备 %s 不支持视频采集", dev_name);
        goto err;
    }
    if (!(caps.capabilities & V4L2_CAP_STREAMING)) {
        LOG_E("设备 %s 不支持 streaming", dev_name);
        goto err;
    }

    /* 设置格式 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = fmt_to_fourcc(format);
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_E("VIDIOC_S_FMT 失败: %s", strerror(errno));
        goto err;
    }
    /* 读取实际格式（驱动可能调整） */
    cap->width = fmt.fmt.pix.width;
    cap->height = fmt.fmt.pix.height;
    LOG_I("V4L2 格式: %ux%u fourcc=0x%08x", cap->width, cap->height,
          fmt.fmt.pix.pixelformat);

    /* 设置帧率 */
    ocr_v4l2_set_fps(cap, fps);

    /* 请求缓冲区 */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = cap->buf_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) < 0) {
        LOG_E("VIDIOC_REQBUFS 失败: %s", strerror(errno));
        goto err;
    }
    cap->buf_count = req.count;
    LOG_I("V4L2 分配缓冲区数: %d", cap->buf_count);

    /* 初始化缓冲池 */
    ocr_pool_init(&cap->pool, cap->buf_count);

    /* 查询每个缓冲区并 mmap，同时导出 DMA-BUF fd */
    for (int i = 0; i < cap->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_E("VIDIOC_QUERYBUF[%d] 失败: %s", i, strerror(errno));
            goto err;
        }
        size_t len = buf.length;
        void *ptr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                         cap->fd, buf.m.offset);
        if (ptr == MAP_FAILED) {
            LOG_E("mmap[%d] 失败: %s", i, strerror(errno));
            goto err;
        }

        /* 导出 DMA-BUF fd */
        int dmabuf_fd = ocr_v4l2_export_dmabuf(cap->fd, i);
        if (dmabuf_fd < 0) {
            LOG_W("导出 DMA-BUF[%d] 失败，后续 RGA 将使用 mmap 地址", i);
        }

        ocr_pool_register(&cap->pool, i, dmabuf_fd, len,
                          cap->width, cap->height, cap->format);
        /* TODO: 保存 mmap 地址供 CPU 访问 */
    }

    atomic_init(&cap->streaming, 0);
    atomic_init(&cap->running, 0);
    return 0;

err:
    close(cap->fd);
    cap->fd = -1;
    return -3;
}

int ocr_v4l2_set_fps(ocr_v4l2_capture_t *cap, uint32_t fps)
{
    if (!cap || cap->fd < 0) return -1;
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (ioctl(cap->fd, VIDIOC_S_PARM, &parm) < 0) {
        LOG_W("VIDIOC_S_PARM 设置帧率失败: %s", strerror(errno));
        return -2;
    }
    return 0;
}

/* 采集线程入口（TODO: 需要外部传入回调或通过 msgbus 通知） */
static void *v4l2_thread(void *arg)
{
    ocr_v4l2_capture_t *cap = (ocr_v4l2_capture_t *)arg;
    LOG_I("采集线程启动");

    fd_set fds;
    struct timeval tv;

    while (atomic_load(&cap->running)) {
        FD_ZERO(&fds);
        FD_SET(cap->fd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(cap->fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_E("select 失败: %s", strerror(errno));
            break;
        }
        if (ret == 0) continue; /* 超时 */

        /* DQBUF */
        struct v4l2_buffer vbuf;
        memset(&vbuf, 0, sizeof(vbuf));
        vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vbuf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(cap->fd, VIDIOC_DQBUF, &vbuf) < 0) {
            if (errno == EAGAIN) continue;
            LOG_E("VIDIOC_DQBUF 失败: %s", strerror(errno));
            break;
        }

        /* 从池中获取对应缓冲并填充时间戳 */
        ocr_buffer_t *buf = &cap->pool.buffers[vbuf.index];
        atomic_store(&buf->refcount, 1);
        buf->timestamp = (uint64_t)vbuf.timestamp.tv_sec * 1000000000ULL +
                         vbuf.timestamp.tv_usec * 1000ULL;
        buf->frame_id = cap->frame_seq++;

        /* TODO: 将 buf 投递到管线下游（rga 节点） */

        /* QBUF 复用 */
        if (ioctl(cap->fd, VIDIOC_QBUF, &vbuf) < 0) {
            LOG_W("VIDIOC_QBUF 失败: %s", strerror(errno));
        }
    }

    LOG_I("采集线程退出");
    return NULL;
}

int ocr_v4l2_start(ocr_v4l2_capture_t *cap)
{
    if (!cap || cap->fd < 0) return -1;

    /* QBUF 所有缓冲 */
    for (int i = 0; i < cap->buf_count; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(cap->fd, VIDIOC_QBUF, &buf) < 0) {
            LOG_E("VIDIOC_QBUF[%d] 失败: %s", i, strerror(errno));
            return -2;
        }
    }

    /* STREAMON */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cap->fd, VIDIOC_STREAMON, &type) < 0) {
        LOG_E("VIDIOC_STREAMON 失败: %s", strerror(errno));
        return -3;
    }
    atomic_store(&cap->streaming, 1);
    atomic_store(&cap->running, 1);

    if (ocr_thread_create(&cap->thread, v4l2_thread, cap) != 0) {
        LOG_E("采集线程创建失败");
        return -4;
    }
    return 0;
}

int ocr_v4l2_stop(ocr_v4l2_capture_t *cap)
{
    if (!cap || cap->fd < 0) return -1;
    atomic_store(&cap->running, 0);
    ocr_thread_join(cap->thread, NULL);

    if (atomic_exchange(&cap->streaming, 0)) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(cap->fd, VIDIOC_STREAMOFF, &type);
    }
    return 0;
}

void ocr_v4l2_close(ocr_v4l2_capture_t *cap)
{
    if (!cap) return;
    ocr_v4l2_stop(cap);
    ocr_pool_destroy(&cap->pool);
    if (cap->fd >= 0) {
        close(cap->fd);
        cap->fd = -1;
    }
}
