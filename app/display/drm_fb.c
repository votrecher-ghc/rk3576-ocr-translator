/**
 * @file drm_fb.c
 * @brief DMA-BUF 导入 DRM framebuffer 与同步 page flip
 *
 * 把 DMA-BUF 导入为 GEM handle，并根据像素格式、stride 和 offset 创建 framebuffer。
 * 导入不复制像素，源 fd 只是借用；翻页接口等待新帧切换完成，供调用方安全释放上一帧。
 */
#include "drm_fb.h"
#include "log.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>

static uint32_t fmt_to_drm_fourcc(ocr_pixel_format_t fmt)
{
    switch (fmt) {
        case OCR_FMT_NV12:      return DRM_FORMAT_NV12;
        case OCR_FMT_NV16:      return DRM_FORMAT_NV16;
        case OCR_FMT_YUYV:      return DRM_FORMAT_YUYV;
        case OCR_FMT_RGB565:    return DRM_FORMAT_RGB565;
        case OCR_FMT_RGB888:    return DRM_FORMAT_RGB888;
        case OCR_FMT_ARGB8888:  return DRM_FORMAT_ARGB8888;
        case OCR_FMT_BGRA8888:  return DRM_FORMAT_BGRA8888;
        default:                return 0;
    }
}

static void fb_reset(ocr_drm_fb_t *fb)
{
    memset(fb, 0, sizeof(*fb));
    fb->dmabuf_fd = -1;
}

static uint64_t plane_end(uint32_t offset, uint32_t pitch,
                          uint32_t rows, uint32_t row_bytes)
{
    if (rows == 0) return offset;
    return (uint64_t)offset + (uint64_t)pitch * (rows - 1U) + row_bytes;
}

/*
 * 生成并校验 linear buffer 布局。本模块没有 modifier 参数，因此明确只支持
 * 线性布局；驱动返回的 bytesperline 应由调用方通过 pitches 传入。
 */
/** @brief 推导并验证 DRM 平面布局及最小缓冲大小，避免内核扫描超出 DMA-BUF 边界。 */
static int build_layout(const ocr_buffer_t *buf,
                        const uint32_t input_pitches[4],
                        const uint32_t input_offsets[4],
                        uint32_t pitches[4], uint32_t offsets[4],
                        uint32_t *plane_count, uint64_t *required_size)
{
    if (!buf || !pitches || !offsets || !plane_count || !required_size ||
        buf->width == 0 || buf->height == 0) {
        return -1;
    }

    memset(pitches, 0, sizeof(uint32_t) * 4);
    memset(offsets, 0, sizeof(uint32_t) * 4);

    uint32_t rows[4] = {0};
    uint32_t row_bytes[4] = {0};

    switch (buf->format) {
        case OCR_FMT_NV12:
            if ((buf->width & 1U) || (buf->height & 1U)) return -2;
            *plane_count = 2;
            row_bytes[0] = row_bytes[1] = buf->width;
            rows[0] = buf->height;
            rows[1] = buf->height / 2U;
            break;
        case OCR_FMT_NV16:
            if (buf->width & 1U) return -2;
            *plane_count = 2;
            row_bytes[0] = row_bytes[1] = buf->width;
            rows[0] = rows[1] = buf->height;
            break;
        case OCR_FMT_YUYV:
            if (buf->width & 1U || buf->width > UINT32_MAX / 2U) return -2;
            *plane_count = 1;
            row_bytes[0] = buf->width * 2U;
            rows[0] = buf->height;
            break;
        case OCR_FMT_RGB565:
            if (buf->width > UINT32_MAX / 2U) return -2;
            *plane_count = 1;
            row_bytes[0] = buf->width * 2U;
            rows[0] = buf->height;
            break;
        case OCR_FMT_RGB888:
            if (buf->width > UINT32_MAX / 3U) return -2;
            *plane_count = 1;
            row_bytes[0] = buf->width * 3U;
            rows[0] = buf->height;
            break;
        case OCR_FMT_ARGB8888:
        case OCR_FMT_BGRA8888:
            if (buf->width > UINT32_MAX / 4U) return -2;
            *plane_count = 1;
            row_bytes[0] = buf->width * 4U;
            rows[0] = buf->height;
            break;
        default:
            return -3;
    }

    /* DRM pitch 的单位是字节，不能传 RGA 的像素 wstride。
     * 没给 input_pitches 才回退到紧密布局；带 padding 的图像必须提供实际值。 */
    for (uint32_t i = 0; i < *plane_count; ++i) {
        pitches[i] = input_pitches ? input_pitches[i] : row_bytes[i];
        if (pitches[i] < row_bytes[i]) return -4;
    }

    if (input_offsets) {
        memcpy(offsets, input_offsets, sizeof(uint32_t) * 4);
    } else if (*plane_count == 2) {
        uint64_t second = (uint64_t)pitches[0] * rows[0];
        if (second > UINT32_MAX) return -5;
        offsets[1] = (uint32_t)second;
    }

    uint64_t end0 = plane_end(offsets[0], pitches[0], rows[0], row_bytes[0]);
    uint64_t end1 = 0;
    if (*plane_count == 2) {
        end1 = plane_end(offsets[1], pitches[1], rows[1], row_bytes[1]);
        /* 线性单 DMA-BUF 的第二平面不能与第一平面重叠。 */
        if ((uint64_t)offsets[1] < end0) return -6;
    }
    /* 同一 fd 包含所有逻辑平面，所需空间是最远平面的末端，而不是简单相加。
     * size=0 的旧接口对象无法在这里验证分配容量，正常池对象应始终提供真实 size。 */
    *required_size = end0 > end1 ? end0 : end1;
    if (buf->size != 0 && *required_size > buf->size) return -7;
    return 0;
}

static void close_gem_handle(ocr_drm_device_t *dev, uint32_t handle)
{
    if (!dev || dev->fd < 0 || handle == 0) return;
    struct drm_gem_close close_arg;
    memset(&close_arg, 0, sizeof(close_arg));
    close_arg.handle = handle;
    (void)drmIoctl(dev->fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
}

/** @brief 借用源 fd 创建 GEM/FB 对象，NV12 的两个逻辑平面共用同一 GEM handle。 */
int ocr_drm_fb_create_with_layout(ocr_drm_device_t *dev,
                                  const ocr_buffer_t *buf,
                                  const uint32_t input_pitches[4],
                                  const uint32_t input_offsets[4],
                                  ocr_drm_fb_t *fb)
{
    if (!dev || !dev->opened || dev->fd < 0 || !buf || buf->fd < 0 || !fb) return -1;
    fb_reset(fb);

    uint32_t fourcc = fmt_to_drm_fourcc(buf->format);
    if (fourcc == 0) return -2;

    uint64_t required_size = 0;
    int ret = build_layout(buf, input_pitches, input_offsets,
                           fb->pitches, fb->offsets,
                           &fb->plane_count, &required_size);
    if (ret != 0) {
        LOG_E("无效 framebuffer 布局: format=%s %ux%u size=%zu ret=%d",
              ocr_pixel_format_str(buf->format), buf->width, buf->height,
              buf->size, ret);
        fb_reset(fb);
        return -3;
    }

    /* fd 是进程可共享的 DMA-BUF 引用；handle 是当前 DRM 文件上下文内的 GEM
     * 标识，两者不是同一种句柄。导入建立内核引用/设备映射，不做 CPU 像素复制。 */
    ret = drmPrimeFDToHandle(dev->fd, buf->fd, &fb->handle);
    if (ret != 0) {
        LOG_E("drmPrimeFDToHandle fd=%d 失败: %s", buf->fd, strerror(errno));
        fb_reset(fb);
        return -4;
    }

    /* NV12/NV16 的 Y 与 UV 平面位于同一个 DMA-BUF，GEM handle 必须复用。 */
    for (uint32_t i = 0; i < fb->plane_count; ++i) {
        fb->handles[i] = fb->handle;
    }

    /* AddFB2 给已有 GEM 内存附加图像解释方式，返回 framebuffer ID。
     * 这个调用还没有把图像送上屏幕；真正的扫描绑定在 modeset/pageflip 中。 */
    ret = drmModeAddFB2(dev->fd, buf->width, buf->height, fourcc,
                        fb->handles, fb->pitches, fb->offsets,
                        &fb->fb_id, 0);
    if (ret != 0) {
        LOG_E("drmModeAddFB2 format=0x%08x 失败: %s", fourcc, strerror(errno));
        close_gem_handle(dev, fb->handle);
        fb_reset(fb);
        return -5;
    }

    fb->width = buf->width;
    fb->height = buf->height;
    fb->format = buf->format;
    fb->dmabuf_fd = buf->fd; /* 仅借用，不在 destroy 中关闭 */

    LOG_D("DRM FB: id=%u %ux%u format=%s handle=%u pitch0=%u offset1=%u required=%llu",
          fb->fb_id, fb->width, fb->height, ocr_pixel_format_str(fb->format),
          fb->handle, fb->pitches[0], fb->offsets[1],
          (unsigned long long)required_size);
    return 0;
}

int ocr_drm_fb_create(ocr_drm_device_t *dev, const ocr_buffer_t *buf,
                      ocr_drm_fb_t *fb)
{
    return ocr_drm_fb_create_with_layout(dev, buf, NULL, NULL, fb);
}

/** @brief 移除 framebuffer 并关闭本模块导入的 GEM handle；不关闭源 DMA-BUF fd。 */
int ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb)
{
    if (!fb) return -1;
    if (fb->fb_id == 0 && fb->handle == 0) {
        fb_reset(fb);
        return 0;
    }
    if (!dev || !dev->opened || dev->fd < 0) return -1;

    if (fb->fb_id != 0) {
        if (drmModeRmFB(dev->fd, fb->fb_id) != 0) {
            LOG_W("drmModeRmFB %u 失败: %s", fb->fb_id, strerror(errno));
            /* framebuffer 仍可能在 scanout，保留 handle 以便调用方切走后重试。 */
            return -2;
        }
    }
    /* 删除的是 DRM framebuffer 和导入句柄，源 DMA-BUF fd 归池所有。
     * 只有上层先让显示离开此图像，才能释放对应的应用引用并允许池复用。 */
    if (fb->handle != 0) {
        close_gem_handle(dev, fb->handle);
    }
    fb_reset(fb);
    return 0;
}

typedef struct {
    int done; /**< 解析到本次翻页完成事件时置 1；由 drmHandleEvent 内回调更新，同线程等待读取。 */
    uint32_t tv_sec; /**< DRM 翻页事件携带的时间戳秒部分，供上层回调使用。 */
    uint32_t tv_usec; /**< 同一翻页时间戳的微秒小数部分，不能直接当纳秒相加。 */
} pageflip_wait_t;

static void pageflip_complete(int fd, unsigned int sequence,
                              unsigned int tv_sec, unsigned int tv_usec,
                              void *user_data)
{
    (void)fd;
    (void)sequence;
    pageflip_wait_t *wait = (pageflip_wait_t *)user_data;
    if (wait) {
        wait->tv_sec = tv_sec;
        wait->tv_usec = tv_usec;
        wait->done = 1;
    }
}

static int crtc_is_scanning_fb(const ocr_drm_device_t *dev, uint32_t fb_id)
{
    drmModeCrtc *crtc = drmModeGetCrtc(dev->fd, dev->crtc_id);
    if (!crtc) return 0;
    int matches = crtc->buffer_id == fb_id;
    drmModeFreeCrtc(crtc);
    return matches;
}

/** @brief 等待 DRM 翻页完成事件；这意味着新帧已接替旧帧，并非新帧已不再被扫描。 */
static int wait_pageflip_event(ocr_drm_device_t *dev, uint32_t fb_id,
                               pageflip_wait_t *wait)
{
    drmEventContext event_ctx;
    struct pollfd pfd;

    memset(&event_ctx, 0, sizeof(event_ctx));
    event_ctx.version = DRM_EVENT_CONTEXT_VERSION;
    /* 回调不由内核直接跳进用户代码，而是在 drmHandleEvent 解析 DRM 事件时调用。
     * drmModePageFlip 提交时传入的 user_data 会随事件回到 pageflip_complete。 */
    event_ctx.page_flip_handler = pageflip_complete;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = dev->fd;
    pfd.events = POLLIN;

    while (!wait->done) {
        /* 此处 poll 的是 DRM 设备 fd，不是图像的 DMA-BUF fd。
         * -1 表示无限等待：当前代码没有翻页超时，停止线程需等本次等待返回。 */
        int ret = poll(&pfd, 1, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            /* 事件读取异常时再查询实际 CRTC：若新 fb 已成为当前扫描源，可确认
             * 已切换；否则不能把事件异常误判成成功，返回失败给上层。 */
            if (crtc_is_scanning_fb(dev, fb_id)) {
                wait->done = 1;
                break;
            }
            LOG_E("等待 page flip 事件失败: %s", strerror(errno));
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (crtc_is_scanning_fb(dev, fb_id)) {
                wait->done = 1;
                break;
            }
            LOG_E("DRM fd 在等待 page flip 时失效 (revents=0x%x)",
                  pfd.revents);
            return -1;
        }
        if (!(pfd.revents & POLLIN)) continue;

        /* POLLIN 仅说明存在 DRM 事件；解析并执行本次 flip 的回调后 done 才置 1。
         * 其他事件不代表这次翻页完成，因此外层继续检查 wait->done。 */
        ret = drmHandleEvent(dev->fd, &event_ctx);
        if (ret != 0) {
            if (errno == EINTR) continue;
            if (crtc_is_scanning_fb(dev, fb_id)) {
                wait->done = 1;
                break;
            }
            LOG_E("处理 page flip 事件失败: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

/** @brief 首帧同步 modeset，后续提交翻页并等待完成后返回，供上层释放旧帧。 */
int ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                        void (*vblank_cb)(int, uint32_t, uint32_t, void *),
                        void *user_data)
{
    if (!dev || !dev->opened || dev->fd < 0 || !dev->crtc ||
        !fb || fb->fb_id == 0) return -1;
    if (fb->width != dev->width || fb->height != dev->height) {
        LOG_E("page flip FB %ux%u 与 mode %ux%u 不匹配",
              fb->width, fb->height, dev->width, dev->height);
        return -2;
    }

    /* 首帧没有可 page-flip 的旧 framebuffer，使用同步 modeset。 */
    if (!dev->mode_set) {
        int ret = ocr_drm_modeset(dev, fb->fb_id);
        if (ret == 0 && vblank_cb) vblank_cb(dev->fd, 0, 0, user_data);
        return ret;
    }

    /* wait 放在当前函数栈上，正常路径必须等待其完成事件被消费后才能返回，
     * 否则提交给内核事件的 &wait 会失去有效生命周期。 */
    pageflip_wait_t wait;
    memset(&wait, 0, sizeof(wait));

    /* 提交成功仅表示请求被接收，不表示屏幕已切换；EVENT 要求内核返回完成通知。
     * 切换点到来后旧帧才退出扫描，新帧成为当前扫描源，还需继续保持引用。 */
    if (drmModePageFlip(dev->fd, dev->crtc_id, fb->fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &wait) != 0) {
        LOG_E("drmModePageFlip crtc=%u fb=%u 失败: %s",
              dev->crtc_id, fb->fb_id, strerror(errno));
        return -3;
    }

    if (wait_pageflip_event(dev, fb->fb_id, &wait) != 0) return -4;
    if (vblank_cb)
        vblank_cb(dev->fd, wait.tv_sec, wait.tv_usec, user_data);
    return 0;
}
