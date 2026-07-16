/** @file v4l2_capture.c @brief V4L2 streaming capture with DMA-BUF export. */
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
    do {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

static void init_v4l2_buffer(const ocr_v4l2_capture_t *cap,
                             struct v4l2_buffer *buffer,
                             struct v4l2_plane planes[VIDEO_MAX_PLANES])
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->type = cap->buffer_type;
    buffer->memory = V4L2_MEMORY_MMAP;
    if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
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
    buffer.index = index;
    return xioctl(cap->fd, VIDIOC_QBUF, &buffer);
}

static int capture_recycle(ocr_buffer_t *buffer, void *user_data)
{
    ocr_v4l2_capture_t *cap = user_data;
    int ret = 0;
    if (!cap || !buffer || !cap->initialized) return -1;

    ocr_mutex_lock(&cap->io_lock);
    if (atomic_load_explicit(&cap->streaming, memory_order_acquire)) {
        if (queue_index_locked(cap, buffer->index) != 0) {
            LOG_E("VIDIOC_QBUF[%u] recycle failed: %s",
                  buffer->index, strerror(errno));
            atomic_store_explicit(&cap->queue_error, 1,
                                  memory_order_release);
            atomic_store_explicit(&cap->running, 0, memory_order_release);
            ret = -1;
        }
    }
    ocr_mutex_unlock(&cap->io_lock);
    return ret;
}

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

int ocr_v4l2_open_ex(ocr_v4l2_capture_t *cap, const char *dev_name,
                     uint32_t width, uint32_t height, uint32_t fps,
                     ocr_pixel_format_t format, int buffer_count)
{
    int io_inited = 0, latest_inited = 0, pool_inited = 0;
    uint32_t requested_fourcc = fmt_to_fourcc(format);

    if (!cap || !dev_name || !*dev_name || width == 0 || height == 0 ||
        width > 16384 || height > 16384 ||
        fps == 0 || requested_fourcc == 0 || buffer_count < 2 ||
        buffer_count > V4L2_MAX_BUFS) {
        return -1;
    }

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

    cap->fd = open(dev_name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (cap->fd < 0) {
        LOG_E("opening V4L2 device %s failed: %s", dev_name, strerror(errno));
        goto fail;
    }
    strncpy(cap->dev_name, dev_name, sizeof(cap->dev_name) - 1);
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

    uint32_t actual_fourcc;
    if (cap->buffer_type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        cap->width = vfmt.fmt.pix.width;
        cap->height = vfmt.fmt.pix.height;
        cap->bytes_per_line = vfmt.fmt.pix.bytesperline;
        cap->size_image = vfmt.fmt.pix.sizeimage;
        actual_fourcc = vfmt.fmt.pix.pixelformat;
    } else {
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
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(cap->fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2 ||
        request.count > V4L2_MAX_BUFS) {
        LOG_E("VIDIOC_REQBUFS failed or returned invalid count %u: %s",
              request.count, strerror(errno));
        goto fail;
    }
    cap->buf_count = (int)request.count;

    if (ocr_pool_init(&cap->pool, cap->buf_count) != 0) goto fail;
    pool_inited = 1;
    if (ocr_pool_set_recycle_callback(&cap->pool, capture_recycle, cap) != 0)
        goto fail;

    for (int i = 0; i < cap->buf_count; ++i) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        init_v4l2_buffer(cap, &buffer, planes);
        buffer.index = (uint32_t)i;
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
        void *mapping = mmap(NULL, length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, cap->fd, offset);
        if (mapping == MAP_FAILED) {
            LOG_E("mmap V4L2 buffer[%d] failed: %s", i, strerror(errno));
            goto fail;
        }

        int dmabuf_fd = ocr_v4l2_export_dmabuf_ex(cap->fd, cap->buffer_type,
                                                   i, 0);
        if (dmabuf_fd < 0) {
            munmap(mapping, length);
            LOG_E("V4L2 buffer[%d] cannot be exported as DMA-BUF", i);
            goto fail;
        }
        if (ocr_pool_register(&cap->pool, i, dmabuf_fd, length,
                              cap->width, cap->height, cap->format) != 0) {
            close(dmabuf_fd);
            munmap(mapping, length);
            goto fail;
        }

        ocr_buffer_t *registered = &cap->pool.buffers[i];
        registered->mmap_addr = mapping;
        uint64_t chroma_offset = (uint64_t)cap->bytes_per_line * cap->height;
        if (chroma_offset > UINT32_MAX) goto fail;
        uint32_t strides[4] = {cap->bytes_per_line, cap->bytes_per_line, 0, 0};
        uint32_t offsets[4] = {0, (uint32_t)chroma_offset, 0, 0};
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
    cap->frame_cb = callback;
    cap->frame_user_data = user_data;
    return 0;
}

ocr_buffer_t *ocr_v4l2_get_latest(ocr_v4l2_capture_t *cap)
{
    if (!cap || !cap->initialized) return NULL;
    ocr_mutex_lock(&cap->latest_lock);
    ocr_buffer_t *buffer = cap->latest_frame;
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
        int ret = xioctl(cap->fd, VIDIOC_DQBUF, &vbuf);
        ocr_mutex_unlock(&cap->io_lock);
        if (ret != 0) {
            if (errno == EAGAIN || errno == EINTR) continue;
            if (!atomic_load_explicit(&cap->running, memory_order_acquire)) break;
            LOG_E("VIDIOC_DQBUF failed: %s", strerror(errno));
            break;
        }

        ocr_buffer_t *buffer = ocr_pool_acquire_index(&cap->pool,
                                                       (int)vbuf.index);
        if (!buffer) {
            LOG_E("V4L2 buffer[%u] was dequeued in an invalid pool state",
                  vbuf.index);
            atomic_store_explicit(&cap->running, 0, memory_order_release);
            break;
        }

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

        ocr_buffer_t *old_latest = NULL;
        if (ocr_buffer_ref(buffer) >= 0) {
            ocr_mutex_lock(&cap->latest_lock);
            old_latest = cap->latest_frame;
            cap->latest_frame = buffer;
            ocr_mutex_unlock(&cap->latest_lock);
        }
        if (old_latest) (void)ocr_buffer_unref(old_latest);

        int transferred = cap->frame_cb ?
            (cap->frame_cb(buffer, cap->frame_user_data) == 0) : 0;
        if (!transferred) (void)ocr_buffer_unref(buffer);
    }

    atomic_store_explicit(&cap->running, 0, memory_order_release);
    LOG_I("V4L2 capture worker stopped");
    return NULL;
}

int ocr_v4l2_start(ocr_v4l2_capture_t *cap)
{
    int idle;

    if (!cap || !cap->initialized || cap->fd < 0) return -1;
    if (atomic_load_explicit(&cap->thread_started, memory_order_acquire)) {
        if (atomic_load_explicit(&cap->running, memory_order_acquire)) return 0;
        if (ocr_v4l2_stop(cap) != 0) return -2;
    }
    if (atomic_load_explicit(&cap->queue_error, memory_order_acquire)) return -2;
    idle = ocr_pool_is_idle(&cap->pool);
    if (idle != 1) {
        LOG_W("V4L2 start deferred: capture buffers are still referenced");
        return idle < 0 ? -2 : -3;
    }

    ocr_mutex_lock(&cap->io_lock);
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

int ocr_v4l2_stop(ocr_v4l2_capture_t *cap)
{
    int failed = 0;

    if (!cap || !cap->initialized) return 0;
    atomic_store_explicit(&cap->running, 0, memory_order_release);

    ocr_mutex_lock(&cap->io_lock);
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

    ocr_mutex_lock(&cap->latest_lock);
    ocr_buffer_t *latest = cap->latest_frame;
    cap->latest_frame = NULL;
    ocr_mutex_unlock(&cap->latest_lock);
    if (latest) (void)ocr_buffer_unref(latest);
    if (atomic_load_explicit(&cap->queue_error, memory_order_acquire)) failed = 1;
    return failed ? -2 : 0;
}

int ocr_v4l2_close(ocr_v4l2_capture_t *cap)
{
    int stop_result;

    if (!cap || !cap->initialized) return 0;
    stop_result = ocr_v4l2_stop(cap);
    if (stop_result == -3) return -3;
    if (stop_result != 0)
        LOG_W("closing V4L2 fd after queue shutdown error");
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
