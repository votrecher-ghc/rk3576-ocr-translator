/**
 * @file drm_overlay.c
 * @brief DRM dumb buffer 上的 FreeType ARGB 文字叠加
 *
 * 文字叠加使用两块 ARGB8888 DRM dumb buffer：CPU 绘制后台缓冲，再提交 overlay plane。
 * 前后缓冲按提交和 vblank 时序交替使用；PRIME fd 仅供可选共享，扫描输出直接使用 framebuffer。
 */
#include "drm_overlay.h"
#include "log.h"

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/** @brief 创建可供 CPU 绘制的 ARGB 缓冲和 framebuffer，PRIME 导出失败不影响本地显示。 */
static int create_dumb_overlay(ocr_drm_overlay_t *overlay, int index)
{
    struct drm_mode_create_dumb create_req;
    memset(&create_req, 0, sizeof(create_req));
    create_req.width = overlay->width;
    create_req.height = overlay->height;
    create_req.bpp = 32;

    if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) != 0) {
        LOG_E("DRM_IOCTL_MODE_CREATE_DUMB 失败: %s", strerror(errno));
        return -1;
    }
    overlay->gem_handle[index] = create_req.handle;
    overlay->pitch[index] = create_req.pitch;

    if (create_req.size == 0 || create_req.size > SIZE_MAX ||
        overlay->pitch[index] < overlay->width * 4U) {
        LOG_E("DRM dumb buffer 返回无效布局: pitch=%u size=%llu",
              overlay->pitch[index], (unsigned long long)create_req.size);
        return -2;
    }
    overlay->pixel_size[index] = (size_t)create_req.size;

    struct drm_mode_map_dumb map_req;
    memset(&map_req, 0, sizeof(map_req));
    map_req.handle = overlay->gem_handle[index];
    if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
        LOG_E("DRM_IOCTL_MODE_MAP_DUMB 失败: %s", strerror(errno));
        return -3;
    }

    void *pixels = mmap(NULL, overlay->pixel_size[index], PROT_READ | PROT_WRITE,
                        MAP_SHARED, overlay->dev->fd, (off_t)map_req.offset);
    if (pixels == MAP_FAILED) {
        LOG_E("mmap DRM dumb buffer 失败: %s", strerror(errno));
        return -4;
    }
    overlay->pixels[index] = pixels;
    memset(overlay->pixels[index], 0, overlay->pixel_size[index]);

    uint32_t handles[4] = { overlay->gem_handle[index], 0, 0, 0 };
    uint32_t pitches[4] = { overlay->pitch[index], 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    if (drmModeAddFB2(overlay->dev->fd, overlay->width, overlay->height,
                      DRM_FORMAT_ARGB8888, handles, pitches, offsets,
                      &overlay->fb_id[index], 0) != 0) {
        LOG_E("drmModeAddFB2(ARGB8888 dumb) 失败: %s", strerror(errno));
        return -5;
    }

    /* PRIME fd 仅用于可选的跨模块共享；scanout 本身直接使用 GEM handle。 */
    int prime_fd = -1;
    if (drmPrimeHandleToFD(overlay->dev->fd, overlay->gem_handle[index],
                           DRM_CLOEXEC | DRM_RDWR, &prime_fd) == 0) {
        overlay->dmabuf_fd[index] = prime_fd;
    } else {
        LOG_D("dumb buffer 不支持 PRIME 导出（非致命）: %s", strerror(errno));
    }
    return 0;
}

int ocr_overlay_init(ocr_drm_overlay_t *overlay, ocr_drm_device_t *dev,
                     ocr_drm_planes_t *planes, const char *font_path,
                     int font_size, uint32_t width, uint32_t height)
{
    if (!overlay || !dev || !dev->opened || dev->fd < 0 ||
        !planes || !planes->overlay ||
        !font_path || font_path[0] == '\0' || font_size <= 0 ||
        width == 0 || height == 0 || width > UINT32_MAX / 4U) {
        return -1;
    }
    if (!planes->overlay->supports_argb) return -2;

    memset(overlay, 0, sizeof(*overlay));
    overlay->dmabuf_fd[0] = -1;
    overlay->dmabuf_fd[1] = -1;
    overlay->active_index = -1;
    overlay->resource_active = 1;
    overlay->dev = dev;
    overlay->planes = planes;
    overlay->width = width;
    overlay->height = height;

    if (ocr_text_renderer_init(&overlay->text, font_path, font_size) != 0) {
        ocr_overlay_destroy(overlay);
        return -3;
    }

    for (int i = 0; i < 2; ++i) {
        int ret = create_dumb_overlay(overlay, i);
        if (ret != 0) {
            ocr_overlay_destroy(overlay);
            return -6;
        }
    }

    overlay->initialized = 1;
    LOG_I("双缓冲叠加层: %ux%u pitch=%u/%u fb=%u/%u 字号=%d",
          width, height, overlay->pitch[0], overlay->pitch[1],
          overlay->fb_id[0], overlay->fb_id[1], font_size);
    return 0;
}

/** @brief 把文字绘制到后台缓冲，逐行寻址使用 DRM 返回的 pitch。 */
int ocr_overlay_render_text(ocr_drm_overlay_t *overlay, const char *text,
                            int x, int y, uint32_t color)
{
    int draw;
    if (!overlay || !overlay->initialized || !text ||
        overlay->draw_index < 0 || overlay->draw_index > 1) {
        return -1;
    }
    draw = overlay->draw_index;
    if (!overlay->pixels[draw]) return -1;
    return ocr_text_renderer_draw(&overlay->text, overlay->pixels[draw],
                                  overlay->width, overlay->height,
                                  overlay->pitch[draw], text, x, y, color);
}

int ocr_overlay_clear(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->initialized ||
        overlay->draw_index < 0 || overlay->draw_index > 1)
        return -1;
    int draw = overlay->draw_index;
    if (!overlay->pixels[draw] || overlay->pixel_size[draw] == 0) return -1;
    memset(overlay->pixels[draw], 0, overlay->pixel_size[draw]);
    return 0;
}

/** @brief 等待选定 CRTC 的下一次 vblank，供前后缓冲交换使用。 */
static int wait_overlay_vblank(ocr_drm_overlay_t *overlay)
{
    drmVBlank vblank;
    memset(&vblank, 0, sizeof(vblank));
    vblank.request.type = DRM_VBLANK_RELATIVE;
    vblank.request.sequence = 1;
#if defined(DRM_VBLANK_HIGH_CRTC_MASK) && defined(DRM_VBLANK_HIGH_CRTC_SHIFT)
    vblank.request.type = (drmVBlankSeqType)(
        vblank.request.type |
        (((unsigned int)overlay->dev->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT) &
         DRM_VBLANK_HIGH_CRTC_MASK));
#else
    if (overlay->dev->crtc_index == 1) {
        vblank.request.type = (drmVBlankSeqType)
            (vblank.request.type | DRM_VBLANK_SECONDARY);
    } else if (overlay->dev->crtc_index > 1) {
        errno = ENOTSUP;
        return -1;
    }
#endif
    for (;;) {
        if (drmWaitVBlank(overlay->dev->fd, &vblank) == 0) return 0;
        if (errno != EINTR) return -1;
    }
}

/** @brief 提交后台文字层并按 vblank 时序交换绘制缓冲，避免立即覆写前台。 */
int ocr_overlay_commit(ocr_drm_overlay_t *overlay)
{
    if (!overlay || !overlay->initialized || !overlay->planes ||
        !overlay->planes->overlay || overlay->draw_index < 0 ||
        overlay->draw_index > 1 || overlay->fb_id[overlay->draw_index] == 0) {
        return -1;
    }

    int next = overlay->draw_index;
    /* 只提交非 scanout buffer；下一帧再绘制刚刚退役的另一块 buffer。
     * CPU 内存屏障约束写入排序，不等同于 DMA_BUF_IOCTL_SYNC；本路径通过
     * DRM dumb buffer 映射绘制，映射的缓存属性由 DRM 驱动决定。 */
    __sync_synchronize();
    int ret = ocr_drm_plane_set_fb_ex(
        overlay->planes, overlay->planes->overlay, overlay->fb_id[next],
        0, 0, overlay->width, overlay->height,
        0, 0, overlay->width, overlay->height);
    if (ret != 0) return ret;
    if (wait_overlay_vblank(overlay) != 0) {
        LOG_W("等待 overlay vblank 失败: %s", strerror(errno));
        /* SetPlane 已提交但切换时点未知，冻结 CPU 绘制以免覆盖 scanout。 */
        overlay->initialized = 0;
        overlay->active_index = -1;
        return -2;
    }
    overlay->active_index = next;
    overlay->draw_index = 1 - next;
    return 0;
}

void ocr_overlay_destroy(ocr_drm_overlay_t *overlay)
{
    if (!overlay) return;
    if (!overlay->resource_active) {
        memset(overlay, 0, sizeof(*overlay));
        overlay->dmabuf_fd[0] = -1;
        overlay->dmabuf_fd[1] = -1;
        overlay->active_index = -1;
        return;
    }

    if (overlay->planes && overlay->planes->overlay &&
        overlay->planes->overlay->in_use) {
        (void)ocr_drm_plane_disable(overlay->planes, overlay->planes->overlay);
    }

    int cleanup_failed = 0;
    for (int i = 0; i < 2; ++i) {
        if (overlay->dev && overlay->dev->opened &&
            overlay->dev->fd >= 0 && overlay->fb_id[i] != 0) {
            if (drmModeRmFB(overlay->dev->fd, overlay->fb_id[i]) != 0) {
                LOG_W("移除 overlay FB %u 失败: %s",
                      overlay->fb_id[i], strerror(errno));
                cleanup_failed = 1;
                continue;
            }
            overlay->fb_id[i] = 0;
        }
        if (overlay->pixels[i] && overlay->pixel_size[i] != 0) {
            munmap(overlay->pixels[i], overlay->pixel_size[i]);
            overlay->pixels[i] = NULL;
        }
        if (overlay->dmabuf_fd[i] >= 0) {
            close(overlay->dmabuf_fd[i]);
            overlay->dmabuf_fd[i] = -1;
        }
        if (overlay->dev && overlay->dev->opened &&
            overlay->dev->fd >= 0 && overlay->gem_handle[i] != 0) {
            struct drm_mode_destroy_dumb destroy_req;
            memset(&destroy_req, 0, sizeof(destroy_req));
            destroy_req.handle = overlay->gem_handle[i];
            if (drmIoctl(overlay->dev->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
                         &destroy_req) != 0) {
                LOG_W("DRM_IOCTL_MODE_DESTROY_DUMB 失败: %s", strerror(errno));
            }
            overlay->gem_handle[i] = 0;
        }
    }
    if (cleanup_failed) return;
    ocr_text_renderer_destroy(&overlay->text);

    memset(overlay, 0, sizeof(*overlay));
    overlay->dmabuf_fd[0] = -1;
    overlay->dmabuf_fd[1] = -1;
    overlay->active_index = -1;
}
