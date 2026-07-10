/**
 * @file drm_plane.c
 * @brief Plane 管理实现
 */
#include "drm_plane.h"
#include "log.h"

#include <string.h>
#include <errno.h>

int ocr_drm_planes_init(ocr_drm_planes_t *planes, ocr_drm_device_t *dev)
{
    if (!planes || !dev || dev->fd < 0) return -1;
    memset(planes, 0, sizeof(*planes));
    planes->dev = dev;

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(dev->fd);
    if (!plane_res) {
        LOG_E("drmModeGetPlaneResources 失败: %s", strerror(errno));
        return -2;
    }

    int count = plane_res->count_planes;
    if (count > DRM_MAX_PLANES) count = DRM_MAX_PLANES;

    for (int i = 0; i < count; i++) {
        drmModePlane *p = drmModeGetPlane(dev->fd, plane_res->planes[i]);
        if (!p) continue;

        ocr_drm_plane_t *pl = &planes->planes[planes->plane_count];
        pl->plane_id = p->plane_id;
        pl->crtc_id = p->crtc_id;
        pl->fb_id = p->fb_id;
        pl->in_use = 0;

        /* 查询 Plane 属性获取类型 */
        drmModeObjectProperties *props = drmModeObjectGetProperties(dev->fd, p->plane_id,
                                                                    DRM_MODE_OBJECT_PLANE);
        if (props) {
            for (uint32_t j = 0; j < props->count_props; j++) {
                drmModePropertyRes *prop = drmModeGetProperty(dev->fd, props->props[j]);
                if (prop && strcmp(prop->name, "type") == 0) {
                    pl->type = (ocr_plane_type_t)props->prop_values[j];
                }
                if (prop) drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }

        /* 分类：Primary 用于底图，Overlay 用于叠加 */
        if (pl->type == OCR_PLANE_PRIMARY && !planes->primary) {
            planes->primary = pl;
            pl->in_use = 1;
        } else if (pl->type == OCR_PLANE_OVERLAY && !planes->overlay) {
            planes->overlay = pl;
            pl->in_use = 1;
        }

        planes->plane_count++;
        drmModeFreePlane(p);

        if (planes->primary && planes->overlay) break;
    }

    drmModeFreePlaneResources(plane_res);
    LOG_I("DRM Plane 枚举完成: primary=%s overlay=%s",
          planes->primary ? "yes" : "no",
          planes->overlay ? "yes" : "no");
    return 0;
}

int ocr_drm_plane_set_fb(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                         uint32_t fb_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h)
{
    if (!planes || !plane || !planes->dev) return -1;
    ocr_drm_device_t *dev = planes->dev;

    int ret = drmModeSetPlane(dev->fd, plane->plane_id, dev->crtc->crtc_id,
                              fb_id, 0,
                              x, y, w, h,
                              0, 0, w << 16, h << 16);
    if (ret < 0) {
        LOG_E("drmModeSetPlane 失败 plane=%u fb=%u: %s",
              plane->plane_id, fb_id, strerror(errno));
        return -2;
    }
    plane->fb_id = fb_id;
    return 0;
}

int ocr_drm_plane_disable(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane)
{
    if (!planes || !plane || !planes->dev) return -1;
    int ret = drmModeSetPlane(planes->dev->fd, plane->plane_id, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0);
    plane->fb_id = 0;
    return ret < 0 ? -1 : 0;
}

void ocr_drm_planes_destroy(ocr_drm_planes_t *planes)
{
    if (!planes) return;
    for (int i = 0; i < planes->plane_count; i++) {
        ocr_drm_plane_disable(planes, &planes->planes[i]);
    }
    memset(planes, 0, sizeof(*planes));
}
