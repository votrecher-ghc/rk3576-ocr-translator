/** @file v4l2_capture.c @brief V4L2 streaming capture with DMA-BUF export.
 *
 * 接口所有权：帧回调成功时接管一个引用，get_latest 返回一个新引用，使用完必须 unref。
 * 采集线程只在 DQBUF 后发布有效帧；缓冲最后一个引用释放后，回收回调才会重新 QBUF。
 */
/*
 * 阅读本文件时，把“驱动队列”和“应用缓冲池”分开看：
 *   驱动队列：QBUF 允许摄像头写；DQBUF 取出摄像头已完成的缓冲。
 *   应用缓冲池：refcount 记录下游还在使用的引用，归零后才允许再次 QBUF。
 * 因此池中 FREE 仅表示“无应用引用”，此时缓冲可能仍在摄像头队列中等待写入。
 *
 * 一块缓冲的完整循环（A 表示内存，i 表示它的固定槽位）：
 *   初始化：REQBUFS 分配 A -> QUERYBUF 查询 A -> mmap -> EXPBUF 导出 A 的 fd。
 *   每一帧：QBUF(i) -> 摄像头写 A -> DQBUF 得到 i -> acquire_index(i)。
 *           把 A 的元数据指针交给 RGA -> RGA 同步用完 -> unref。
 *           latest_frame 等持有者也释放 -> 最后一次 unref -> recycle -> QBUF(i)。
 * EXPBUF 和 mmap 只在初始化时建立共享引用/映射，逐帧传递不复制像素。
 */
#include "v4l2_capture.h"
#include "v4l2_dmabuf.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static uint32_t fmt_to_fourcc(ocr_pixel_format_t fmt)
{
    /* 内部枚举不是 V4L2 FourCC，必须在 ioctl 边界显式转换。
     * 返回 0 表示不支持，由 open_ex 在分配资源前拒绝。 */
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

static uint32_t default_stride(ocr_pixel_format_t format, uint32_t width)
{
    /* 仅供驱动未报告 bytesperline 时回退到紧密行布局。
     * NV12/NV16 此处计算 Y 平面跨度，不是整幅图像平均每像素的字节数。 */
    switch (format) {
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565:   return width * 2U;
    case OCR_FMT_RGB888:   return width * 3U;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888: return width * 4U;
    default:               return width;
    }
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    /* 信号打断不代表设备操作失败，EINTR 时原请求重试。
     * EAGAIN 则交给采集循环处理，不能在这里忙等一个尚未采集完成的帧。 */
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

/** @brief 统一初始化单平面/多平面队列参数；当前只接受一个 V4L2 内存平面。 */
static void init_v4l2_buffer(const ocr_v4l2_capture_t *cap,
                             struct v4l2_buffer *buffer,
                             struct v4l2_plane planes[VIDEO_MAX_PLANES])
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->type = cap->buffer_type;
    buffer->memory = V4L2_MEMORY_MMAP;
    if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        /* MPLANE API 要求用 m.planes 指向平面描述数组；length 是数组元素数。
         * 本项目只支持一个 V4L2 内存平面；其中的 NV12 Y/UV 仍是两个逻辑平面。 */
        memset(planes, 0, sizeof(struct v4l2_plane) * VIDEO_MAX_PLANES);
        buffer->m.planes = planes;
        buffer->length = 1;
    }
}

static int queue_index_locked(ocr_v4l2_capture_t *cap, uint32_t index)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    init_v4l2_buffer(cap, &buffer, planes);
    /* MMAP 队列按索引交接缓冲；导出的 DMA-BUF fd 用于下游共享，不填到这里。 */
    buffer.index = index;
    return xioctl(cap->fd, VIDIOC_QBUF, &buffer);
}

static int capture_recycle(ocr_buffer_t *buffer, void *user_data)
{
    ocr_v4l2_capture_t *cap = user_data;
    int ret = 0;
    if (!cap || !buffer || !cap->initialized) return -1;

    /* 此函数在最后一次 unref 的线程中执行，可能是 RGA 线程，也可能是
     * 替换 latest_frame 的采集线程；它不是固定由摄像头驱动调用的回调。
     * 上层已持有 pool.lock，此处再取 io_lock，串行化 QBUF 与流启停。 */
    ocr_mutex_lock(&cap->io_lock);
    if (atomic_load_explicit(&cap->streaming, memory_order_acquire)) {
        if (queue_index_locked(cap, buffer->index) != 0) {
            /* QBUF 失败后，该缓冲虽然已无应用引用，却没有成功归还驱动。
             * 标记队列错误并让采集线程退出，避免继续按“所有槽都在正常轮转”运行。 */
            LOG_E("VIDIOC_QBUF[%u] recycle failed: %s",
                  buffer->index, strerror(errno));
            atomic_store_explicit(&cap->queue_error, 1,
                                  memory_order_release);
            atomic_store_explicit(&cap->running, 0, memory_order_release);
            ret = -1;
        }
    }
    /* STREAMOFF 后释放旧帧只回收应用状态，不再 QBUF；下次 start 统一重新排队。 */
    ocr_mutex_unlock(&cap->io_lock);
    return ret;
}

/** @brief 按实际布局计算有效图像的最小字节范围，用于拒绝不完整采集帧。 */
static uint64_t minimum_payload_size(const ocr_buffer_t *buffer)
{
    uint32_t rows[2] = {buffer->height, 0};
    uint32_t row_bytes[2] = {0, 0};
    int planes = 1;
    switch (buffer->format) {
    case OCR_FMT_NV12:
        planes = 2; rows[1] = buffer->height / 2U;
        row_bytes[0] = row_bytes[1] = buffer->width;
        break;
    case OCR_FMT_NV16:
        planes = 2; rows[1] = buffer->height;
        row_bytes[0] = row_bytes[1] = buffer->width;
        break;
    case OCR_FMT_YUYV:
    case OCR_FMT_RGB565: row_bytes[0] = buffer->width * 2U; break;
    case OCR_FMT_RGB888: row_bytes[0] = buffer->width * 3U; break;
    case OCR_FMT_ARGB8888:
    case OCR_FMT_BGRA8888: row_bytes[0] = buffer->width * 4U; break;
    default: return UINT64_MAX;
    }
    if ((int)buffer->plane_count < planes) return UINT64_MAX;

    uint64_t end = 0;
    for (int i = 0; i < planes; ++i) {
        if (rows[i] == 0 || buffer->strides[i] < row_bytes[i])
            return UINT64_MAX;
        /* 最后一个有效字节之后的位置 = 平面起点 + 前面各行的完整跨度 + 末行像素。
         * 末行尾部填充不属于必需 payload，不能简单用 stride * rows 来判断短帧。 */
        uint64_t plane_end = (uint64_t)buffer->offsets[i] +
            (uint64_t)buffer->strides[i] * (rows[i] - 1U) + row_bytes[i];
        if (plane_end > end) end = plane_end;
    }
    return end;
}

int ocr_v4l2_open(ocr_v4l2_capture_t *cap, const char *dev_name,
                  uint32_t width, uint32_t height, uint32_t fps,
                  ocr_pixel_format_t format)
{
    return ocr_v4l2_open_ex(cap, dev_name, width, height, fps, format, 4);
}

/** @brief 协商采集格式并分配、映射、导出所有缓冲；失败时撤销已获取的资源。 */
int ocr_v4l2_open_ex(ocr_v4l2_capture_t *cap, const char *dev_name,
                     uint32_t width, uint32_t height, uint32_t fps,
                     ocr_pixel_format_t format, int buffer_count)
{
    /* 初始化可在任一步失败，三个标志让 fail 路径只销毁真正建成的锁和池。 */
    int io_inited = 0, latest_inited = 0, pool_inited = 0;
    //将ocr格式转为v4l2的fourcc格式
    uint32_t requested_fourcc = fmt_to_fourcc(format);

    if (!cap || !dev_name || !*dev_name || width == 0 || height == 0 ||
        width > 16384 || height > 16384 ||
        fps == 0 || requested_fourcc == 0 || buffer_count < 2 ||
        buffer_count > V4L2_MAX_BUFS) {
        return -1;
    }

    /* fd=0 是合法描述符，不能用全零结构表示“设备未打开”；明确置为 -1。
     * streaming 表示内核采集流状态，running 表示线程应否继续，thread_started
     * 表示 pthread 句柄仍需 join；三者不是同一个状态。 */
    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;
    atomic_init(&cap->streaming, 0);
    atomic_init(&cap->running, 0);
    atomic_init(&cap->thread_started, 0);
    atomic_init(&cap->queue_error, 0);
    if (ocr_mutex_init(&cap->io_lock) != 0) goto fail;
    io_inited = 1;
    if (ocr_mutex_init(&cap->latest_lock) != 0) goto fail;
    latest_inited = 1;

    /* NONBLOCK 使 DQBUF 在暂时无帧时返回 EAGAIN，线程可检查退出标志。（打开时设置模式，后续取帧时按这个模式执行）
     * CLOEXEC 防止未来启动子程序时意外继承摄像头设备。 */
    cap->fd = open(dev_name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cap->fd < 0) {
        LOG_E("opening V4L2 device %s failed: %s", dev_name, strerror(errno));
        goto fail;
    }
    strncpy(cap->dev_name, dev_name, sizeof(cap->dev_name) - 1);
    //字符串大于63时，不会在结尾加'/0'，所以需要手动补上
    cap->dev_name[sizeof(cap->dev_name) - 1] = '\0';
    cap->width = width;
    cap->height = height;
    cap->fps = fps;
    cap->format = format;
    cap->buf_count = buffer_count;

    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &capability) != 0) {
        LOG_E("VIDIOC_QUERYCAP failed: %s", strerror(errno));
        goto fail;
    }
    /* 一个物理设备可能暴露多个 video 节点；有 DEVICE_CAPS 时以本节点能力为准，
     * 避免把同一设备其他节点的采集/输出能力误认成当前节点支持的能力。 */
    uint32_t caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ?
                    capability.device_caps : capability.capabilities;
    if (!(caps & V4L2_CAP_STREAMING)) {
        LOG_E("V4L2 device does not support streaming");
        goto fail;
    }
    if (caps & V4L2_CAP_VIDEO_CAPTURE) {
        cap->buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    } else if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) {
        cap->buffer_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    } else {
        LOG_E("V4L2 device has no capture queue");
        goto fail;
    }

    /* S_FMT 是格式协商，不保证驱动逐字接受请求；输入用期望参数，返回用实际参数。
     * FIELD_NONE 请求逐行图像，下面按协商后的宽高和 stride 创建共享图像元数据。 */
    struct v4l2_format vfmt;
    memset(&vfmt, 0, sizeof(vfmt));
    vfmt.type = cap->buffer_type;
    if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        vfmt.fmt.pix.width = width;
        vfmt.fmt.pix.height = height;
        vfmt.fmt.pix.pixelformat = requested_fourcc;
        vfmt.fmt.pix.field = V4L2_FIELD_NONE;
    } else {
        vfmt.fmt.pix_mp.width = width;
        vfmt.fmt.pix_mp.height = height;
        vfmt.fmt.pix_mp.pixelformat = requested_fourcc;
        vfmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    }
    if (xioctl(cap->fd, VIDIOC_S_FMT, &vfmt) != 0) {
        LOG_E("VIDIOC_S_FMT failed: %s", strerror(errno));
        goto fail;
    }

    /* 不能继续沿用函数入参 width/height：驱动可能为了硬件对齐调整实际尺寸。
     * pix 与 pix_mp 是 union 的不同成员，按队列类型读取，不能混用。 */
    uint32_t actual_fourcc;
    if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        cap->width = vfmt.fmt.pix.width;
        cap->height = vfmt.fmt.pix.height;
        cap->bytes_per_line = vfmt.fmt.pix.bytesperline;
        cap->size_image = vfmt.fmt.pix.sizeimage;
        actual_fourcc = vfmt.fmt.pix.pixelformat;
    } else {
        /* 真正的多内存平面通常需要多个 DMA-BUF fd；当前 ocr_buffer_t 只有一个 fd，
         * 所以不能把 plane 0 的 fd 假装成包含其余平面的完整图像。 */
        if (vfmt.fmt.pix_mp.num_planes != 1) {
            LOG_E("multi-planar V4L2 queue exposes %u DMA planes; only one contiguous plane is supported",
                  vfmt.fmt.pix_mp.num_planes);
            goto fail;
        }
        cap->width = vfmt.fmt.pix_mp.width;
        cap->height = vfmt.fmt.pix_mp.height;
        cap->bytes_per_line = vfmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        cap->size_image = vfmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        actual_fourcc = vfmt.fmt.pix_mp.pixelformat;
    }
    /* 允许驱动调整尺寸，但不接受静默替换像素格式；下游按 cap->format 解读数据，
     * 若实际 FourCC 不符，会使 RGA 把错误的字节布局当成 NV12 等格式。 */
    if (actual_fourcc != requested_fourcc) {
        LOG_E("V4L2 changed requested pixel format 0x%08x to 0x%08x",
              requested_fourcc, actual_fourcc);
        goto fail;
    }
    if (cap->bytes_per_line == 0)
        cap->bytes_per_line = default_stride(format, cap->width);
    LOG_I("V4L2 format: %ux%u stride=%u size=%u fourcc=0x%08x",
          cap->width, cap->height, cap->bytes_per_line,
          cap->size_image, actual_fourcc);

    (void)ocr_v4l2_set_fps(cap, fps);

    struct v4l2_requestbuffers request;
    memset(&request, 0, sizeof(request));
    request.count = (uint32_t)buffer_count;
    request.type = cap->buffer_type;
    /* 摄像头驱动分配采集内存；随后通过 EXPBUF 导出，而非导入外部分配的 fd。 */
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2 ||
        request.count > V4L2_MAX_BUFS) {
        LOG_E("VIDIOC_REQBUFS failed or returned invalid count %u: %s",
              request.count, strerror(errno));
        goto fail;
    }
    /* count 是输入期望数量，也是输出实际数量；驱动可以增减，后续以返回值为准。 */
    cap->buf_count = (int)request.count;

    if (ocr_pool_init(&cap->pool, cap->buf_count) != 0) goto fail;
    pool_inited = 1;
    /* pool 内每个 buffer 的 release_cb 已指向 pool_recycle_buffer。
     * 这里再挂整池通知，形成 unref -> 池回收 -> capture_recycle -> QBUF 的链条。 */
    if (ocr_pool_set_recycle_callback(&cap->pool, capture_recycle, cap) != 0)
        goto fail;

    for (int i = 0; i < cap->buf_count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        init_v4l2_buffer(cap, &buffer, planes);
        buffer.index = (uint32_t)i;
        /* REQBUFS 已分配实际内存；QUERYBUF 只查询第 i 块的长度和 mmap 所需偏移。
         * m.offset/mem_offset 是驱动映射用的偏移值，不是物理地址或 CPU 指针。 */
        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            LOG_E("VIDIOC_QUERYBUF[%d] failed: %s", i, strerror(errno));
            goto fail;
        }

        size_t length;
        off_t offset;
        if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
            length = buffer.length;
            offset = (off_t)buffer.m.offset;
        } else {
            if (buffer.length != 1) {
                LOG_E("VIDIOC_QUERYBUF[%d] returned %u planes", i, buffer.length);
                goto fail;
            }
            length = planes[0].length;
            offset = (off_t)planes[0].m.mem_offset;
        }
        /* 映射的是同一块采集内存；CPU 地址和后面导出的 fd 都引用它。 */
        void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, cap->fd, offset);
        if (mapping == MAP_FAILED) {
            LOG_E("mmap V4L2 buffer[%d] failed: %s", i, strerror(errno));
            goto fail;
        }

        /* 导出仅在初始化时进行；每帧复用 fd，不再分配或复制整幅图像。 */
        int dmabuf_fd = ocr_v4l2_export_dmabuf_ex(cap->fd, cap->buffer_type,
                                                   i, 0);
        if (dmabuf_fd < 0) {
            munmap(mapping, length);
            LOG_E("V4L2 buffer[%d] cannot be exported as DMA-BUF", i);
            goto fail;
        }
        /* 成功前 fd 和 mapping 都归当前函数；注册成功后 fd 交给池，随后把 mapping
         * 也记入池对象。失败分支必须先自行 close/munmap，避免漏掉尚未移交的资源。 */
        if (ocr_pool_register(&cap->pool, i, dmabuf_fd, length,
                              cap->width, cap->height, cap->format) != 0) {
            close(dmabuf_fd);
            munmap(mapping, length);
            goto fail;
        }

        ocr_buffer_t *registered = &cap->pool.buffers[i];
        registered->mmap_addr = mapping;
        /* 本实现约定 Y 平面占 stride*height，UV 紧接其后。
         * 例如 NV12 1280x720、stride=1280：UV 从 921600 字节处开始；
         * 若 stride 大于宽度，必须使用 stride 计算，不能仍按 width*height 寻址。 */
        uint64_t chroma_offset = (uint64_t)cap->bytes_per_line * cap->height;
        if (chroma_offset > UINT32_MAX) goto fail;
        uint32_t strides[4] = {cap->bytes_per_line, cap->bytes_per_line, 0, 0};
        uint32_t offsets[4] = {0, (uint32_t)chroma_offset, 0, 0};
        /* NV12/NV16 的 Y、UV 是两个逻辑平面，仍共用一个 DMA-BUF fd。 */
        uint32_t logical_planes =
            (format == OCR_FMT_NV12 || format == OCR_FMT_NV16) ? 2U : 1U;
        if (ocr_buffer_set_layout(registered, logical_planes,
                                  strides, offsets) != 0) {
            LOG_E("invalid image layout for V4L2 buffer[%d]", i);
            goto fail;
        }
    }

    cap->initialized = 1;
    return 0;

fail:
    /* 此时尚未 STREAMON，也未向下游发布引用，已注册槽位均可由池销毁。
     * 已注册项先 munmap/close 导出 fd，最后关闭 video fd 释放驱动分配的采集资源。 */
    if (pool_inited) (void)ocr_pool_destroy(&cap->pool);
    if (cap->fd >= 0) close(cap->fd);
    if (latest_inited) ocr_mutex_destroy(&cap->latest_lock);
    if (io_inited) ocr_mutex_destroy(&cap->io_lock);
    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;
    return -2;
}

int ocr_v4l2_set_fps(ocr_v4l2_capture_t *cap, uint32_t fps)
{
    if (!cap || cap->fd < 0 || fps == 0) return -1;
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = cap->buffer_type;
    /* V4L2 用帧间隔分数描述帧率：1/fps 秒一帧。驱动返回值可能与请求值不同。 */
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fps;
    if (xioctl(cap->fd, VIDIOC_S_PARM, &parm) != 0) {
        LOG_W("VIDIOC_S_PARM failed: %s", strerror(errno));
        return -2;
    }
    if (parm.parm.capture.timeperframe.numerator != 0) {
        cap->fps = parm.parm.capture.timeperframe.denominator /
                   parm.parm.capture.timeperframe.numerator;
    } else {
        cap->fps = fps;
    }
    return 0;
}

int ocr_v4l2_set_frame_callback(ocr_v4l2_capture_t *cap,
                                ocr_v4l2_frame_cb_t callback,
                                void *user_data)
{
    if (!cap || !cap->initialized ||
        atomic_load_explicit(&cap->thread_started, memory_order_acquire)) {
        return -1;
    }
    /* 只允许在线程启动前变更回调；运行中无锁替换函数指针/上下文会造成组合不一致。 */
    cap->frame_cb = callback;
    cap->frame_user_data = user_data;
    return 0;
}

ocr_buffer_t *ocr_v4l2_get_latest(ocr_v4l2_capture_t *cap)
{
    if (!cap || !cap->initialized) return NULL;
    ocr_mutex_lock(&cap->latest_lock);
    ocr_buffer_t *buffer = cap->latest_frame;
    /* 调用者得到独立引用，用完必须 unref；加锁避免取指针时旧帧已经被回收。 */
    if (buffer && ocr_buffer_ref(buffer) < 0) buffer = NULL;
    ocr_mutex_unlock(&cap->latest_lock);
    return buffer;
}

static void *v4l2_thread(void *arg)
{
    ocr_v4l2_capture_t *cap = arg;
    LOG_I("V4L2 capture worker started");

    while (atomic_load_explicit(&cap->running, memory_order_acquire)) {
        fd_set readfds;
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        FD_ZERO(&readfds);
        FD_SET(cap->fd, &readfds);
        /* 最多等待一秒，超时后重新检查 running；select 的 fd 集会被系统调用改写，
         * 因而每轮都重建 readfds/timeout，不直接复用上一次的返回结果。 */
        int ready = select(cap->fd + 1, &readfds, NULL, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            LOG_E("V4L2 select failed: %s", strerror(errno));
            break;
        }
        if (ready == 0 || !atomic_load_explicit(&cap->running,
                                                memory_order_acquire)) {
            continue;
        }

        struct v4l2_buffer vbuf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        init_v4l2_buffer(cap, &vbuf, planes);
        ocr_mutex_lock(&cap->io_lock);
        /* 同步点一：DQBUF 取出已采集的帧，随后验证错误标记和有效数据长度。
         * select 只表示可以尝试取帧，实际可用的缓冲索引以 DQBUF 返回值为准。 */
        int ret = xioctl(cap->fd, VIDIOC_DQBUF, &vbuf);
        ocr_mutex_unlock(&cap->io_lock);
        if (ret != 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (!atomic_load_explicit(&cap->running, memory_order_acquire)) break;
            LOG_E("VIDIOC_DQBUF failed: %s", strerror(errno));
            break;
        }

        /* 采集池的 FREE 表示无应用引用，不表示摄像头已经写完；必须先 DQBUF。 */
        ocr_buffer_t *buffer = ocr_pool_acquire_index(&cap->pool,
                                                       (int)vbuf.index);
        if (!buffer) {
            LOG_E("V4L2 buffer[%u] was dequeued in an invalid pool state",
                  vbuf.index);
            atomic_store_explicit(&cap->running, 0, memory_order_release);
            break;
        }

        /* 分配长度 length 与本帧有效长度 bytesused 不同。MPLANE 的有效长度及
         * 数据起点位于 planes[0]；当前只接受 data_offset=0 的既定布局。 */
        uint32_t bytes_used = vbuf.bytesused;
        uint32_t data_offset = 0;
        if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            bytes_used = planes[0].bytesused;
            data_offset = planes[0].data_offset;
        }
        uint64_t minimum = minimum_payload_size(buffer);
        if ((vbuf.flags & V4L2_BUF_FLAG_ERROR) || data_offset != 0 ||
            minimum == UINT64_MAX || bytes_used < minimum) {
            LOG_W("丢弃无效 V4L2 帧 index=%u flags=0x%x bytes=%u min=%llu offset=%u",
                  vbuf.index, vbuf.flags, bytes_used,
                  (unsigned long long)minimum, data_offset);
            (void)ocr_buffer_unref(buffer); /* 回调立即 QBUF */
            continue;
        }

        /* 防抖姿态历史使用单调时钟纳秒；V4L2 timeval 的微秒部分需要乘 1000。
         * 驱动未声明 MONOTONIC 时，以取帧时刻回退；它不等同于真实曝光时刻。 */
        if (vbuf.flags & V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC) {
            buffer->timestamp = (uint64_t)vbuf.timestamp.tv_sec * 1000000000ULL +
                                (uint64_t)vbuf.timestamp.tv_usec * 1000ULL;
        } else {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            buffer->timestamp = (uint64_t)now.tv_sec * 1000000000ULL +
                                (uint64_t)now.tv_nsec;
        }
        buffer->frame_id = cap->frame_seq++;

        /* 最新帧缓存额外保留一个引用，供拍照或状态查询安全地借用。 */
        ocr_buffer_t *old_latest = NULL;
        if (ocr_buffer_ref(buffer) >= 0) {
            ocr_mutex_lock(&cap->latest_lock);
            old_latest = cap->latest_frame;
            cap->latest_frame = buffer;
            ocr_mutex_unlock(&cap->latest_lock);
        }
        /* 先解 latest_lock 再 unref：旧帧若恰好归零，会进入 pool.lock -> io_lock。
         * 保持引用回收在指针锁之外，避免把设备 ioctl 塞进最新帧的临界区。 */
        if (old_latest) (void)ocr_buffer_unref(old_latest);

        /* 成功回调接管当前引用。这里只传缓冲指针及 fd，像素仍留在采集内存中。 */
        int transferred = cap->frame_cb ?
            (cap->frame_cb(buffer, cap->frame_user_data) == 0) : 0;
        if (!transferred) (void)ocr_buffer_unref(buffer);
    }

    atomic_store_explicit(&cap->running, 0, memory_order_release);
    LOG_I("V4L2 capture worker stopped");
    return NULL;
}

/** @brief 先把采集缓冲排入驱动队列，再 STREAMON 并创建采集线程。 */
int ocr_v4l2_start(ocr_v4l2_capture_t *cap)
{
    int idle;

    if (!cap || !cap->initialized || cap->fd < 0) return -1;
    /* 上次线程已退出也必须先 join，回收句柄并关闭旧流，才能重新创建采集线程。 */
    if (atomic_load_explicit(&cap->thread_started, memory_order_acquire)) {
        if (atomic_load_explicit(&cap->running, memory_order_acquire)) return 0;
        if (ocr_v4l2_stop(cap) != 0) return -2;
    }
    if (atomic_load_explicit(&cap->queue_error, memory_order_acquire)) return -2;
    /* 重启将把所有槽位重新 QBUF；任一旧帧仍被 RGA/拍照持有时都不能这样做，
     * 否则摄像头会覆盖使用者正在读取的内存。 */
    idle = ocr_pool_is_idle(&cap->pool);
    if (idle != 1) {
        LOG_W("V4L2 start deferred: capture buffers are still referenced");
        return idle < 0 ? -2 : -3;
    }

    ocr_mutex_lock(&cap->io_lock);
    /* 初次排队发生在 STREAMON 之前，驱动先拿到可写缓冲再开始采集。
     * QBUF 不创建应用引用，所以这些槽位在应用池中仍保持 FREE/refcount=0。 */
    for (int i = 0; i < cap->buf_count; ++i) {
        if (queue_index_locked(cap, (uint32_t)i) != 0) {
            atomic_store_explicit(&cap->queue_error, 1,
                                  memory_order_release);
            ocr_mutex_unlock(&cap->io_lock);
            LOG_E("initial VIDIOC_QBUF[%d] failed: %s", i, strerror(errno));
            return -2;
        }
    }
    enum v4l2_buf_type type = (enum v4l2_buf_type)cap->buffer_type;
    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) != 0) {
        atomic_store_explicit(&cap->queue_error, 1, memory_order_release);
        ocr_mutex_unlock(&cap->io_lock);
        LOG_E("VIDIOC_STREAMON failed: %s", strerror(errno));
        return -3;
    }
    atomic_store_explicit(&cap->streaming, 1, memory_order_release);
    atomic_store_explicit(&cap->running, 1, memory_order_release);
    ocr_mutex_unlock(&cap->io_lock);

    /* 先置 running 再创建线程，避免新线程刚开始就读到 0 而立即退出。
     * 创建失败时流已经开了，因此必须 STREAMOFF 回滚，不能只清线程标志。 */
    if (ocr_thread_create(&cap->thread, v4l2_thread, cap) != 0) {
        int streamoff_result;

        atomic_store_explicit(&cap->running, 0, memory_order_release);
        ocr_mutex_lock(&cap->io_lock);
        streamoff_result = xioctl(cap->fd, VIDIOC_STREAMOFF, &type);
        if (streamoff_result != 0 && errno != EINVAL) {
            atomic_store_explicit(&cap->queue_error, 1,
                                  memory_order_release);
        } else {
            atomic_store_explicit(&cap->queue_error, 0,
                                  memory_order_release);
        }
        atomic_store_explicit(&cap->streaming, 0, memory_order_release);
        ocr_mutex_unlock(&cap->io_lock);
        return -4;
    }
    atomic_store_explicit(&cap->thread_started, 1, memory_order_release);
    return 0;
}

/** @brief 停止流并回收线程，释放最新帧引用；外部持有的帧仍须由其使用者归还。 */
int ocr_v4l2_stop(ocr_v4l2_capture_t *cap)
{
    int failed = 0;

    if (!cap || !cap->initialized) return 0;
    atomic_store_explicit(&cap->running, 0, memory_order_release);

    ocr_mutex_lock(&cap->io_lock);
    /* 持 io_lock 把 streaming 从 1 改成 0 后再关流：后续回收回调会看到停止状态，
     * 不会在 STREAMOFF 之后又把最后释放的旧帧重新 QBUF。 */
    if (atomic_exchange_explicit(&cap->streaming, 0, memory_order_acq_rel)) {
        enum v4l2_buf_type type = (enum v4l2_buf_type)cap->buffer_type;
        if (xioctl(cap->fd, VIDIOC_STREAMOFF, &type) != 0 && errno != EINVAL) {
            LOG_W("VIDIOC_STREAMOFF failed: %s", strerror(errno));
            atomic_store_explicit(&cap->queue_error, 1,
                                  memory_order_release);
            failed = 1;
        } else {
            atomic_store_explicit(&cap->queue_error, 0,
                                  memory_order_release);
        }
    }
    ocr_mutex_unlock(&cap->io_lock);

    if (atomic_load_explicit(&cap->thread_started, memory_order_acquire)) {
        if (pthread_equal(cap->thread, pthread_self())) return -3;
        if (ocr_thread_join(cap->thread, NULL) != 0) return -3;
        atomic_store_explicit(&cap->thread_started, 0, memory_order_release);
    }

    /* join 后采集线程不会再发布最新帧；先取走指针，再在锁外释放缓存持有的引用。 */
    ocr_mutex_lock(&cap->latest_lock);
    ocr_buffer_t *latest = cap->latest_frame;
    cap->latest_frame = NULL;
    ocr_mutex_unlock(&cap->latest_lock);
    if (latest) (void)ocr_buffer_unref(latest);
    if (atomic_load_explicit(&cap->queue_error, memory_order_acquire)) failed = 1;
    return failed ? -2 : 0;
}

/** @brief 停止采集并销毁缓冲池；仍有引用时保留资源，允许归还后重试。 */
int ocr_v4l2_close(ocr_v4l2_capture_t *cap)
{
    int stop_result;

    if (!cap || !cap->initialized) return 0;
    stop_result = ocr_v4l2_stop(cap);
    if (stop_result == -3) return -3;
    if (stop_result != 0)
        LOG_W("closing V4L2 fd after queue shutdown error");
    /* STREAMOFF 只停止设备写入，不能使下游手上的指针失效。
     * 池忙时拒绝关闭，保留 fd、映射和回调上下文，待下游归还后才能重试。 */
    if (ocr_pool_destroy(&cap->pool) != 0) {
        LOG_W("V4L2 close deferred: downstream still owns capture buffers");
        return -2;
    }
    if (cap->fd >= 0) close(cap->fd);
    ocr_mutex_destroy(&cap->latest_lock);
    ocr_mutex_destroy(&cap->io_lock);
    memset(cap, 0, sizeof(*cap));
    cap->fd = -1;
    return 0;
}
