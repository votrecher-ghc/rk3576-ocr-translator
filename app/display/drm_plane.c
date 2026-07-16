/**
 * @file drm_plane.c
 * @brief DRM Plane 能力筛选与 legacy KMS 设置
 */
#include "drm_plane.h"
#include "log.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ocr_drm_plane_t plane;
    int type_known;
} plane_candidate_t;

static int plane_has_format(const drmModePlane *plane, uint32_t format)
{
    for (uint32_t i = 0; plane && i < plane->count_formats; ++i) {
        if (plane->formats[i] == format) return 1;
    }
    return 0;
}

static int enum_is_premultiplied(const char *name)
{
    return name &&
           (strcmp(name, "Pre-multiplied") == 0 ||
            strcmp(name, "Pre-Multiplied") == 0 ||
            strcmp(name, "premultiplied") == 0);
}

static void read_plane_properties(int fd, uint32_t plane_id,
                                  plane_candidate_t *candidate)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    if (!props) return;

    for (uint32_t i = 0; i < props->count_props; ++i) {
        drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
        if (!prop) continue;

        uint64_t value = props->prop_values[i];
        if (strcmp(prop->name, "type") == 0) {
            candidate->plane.type = (ocr_plane_type_t)value;
            candidate->type_known = 1;
        } else if (strcmp(prop->name, "zpos") == 0) {
            candidate->plane.zpos_prop_id = prop->prop_id;
            candidate->plane.zpos = value;
            candidate->plane.zpos_min = value;
            candidate->plane.zpos_max = value;
            if (prop->count_values >= 2) {
                candidate->plane.zpos_min = prop->values[0];
                candidate->plane.zpos_max = prop->values[1];
            }
        } else if (strcmp(prop->name, "pixel blend mode") == 0) {
            candidate->plane.blend_value = value;
            for (int e = 0; e < prop->count_enums; ++e) {
                if (enum_is_premultiplied(prop->enums[e].name)) {
                    candidate->plane.blend_prop_id = prop->prop_id;
                    candidate->plane.premult_value = prop->enums[e].value;
                    break;
                }
            }
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
}

static int base_score(const plane_candidate_t *candidate, uint32_t crtc_id)
{
    if (!candidate->plane.supports_nv12) return -1;
    /* 首帧 modeset/pageflip 绑定的是 KMS Primary plane。 */
    if (candidate->type_known && candidate->plane.type != OCR_PLANE_PRIMARY) return -1;

    int score = 20;
    if (candidate->type_known && candidate->plane.type == OCR_PLANE_PRIMARY) score += 120;
    if (candidate->plane.crtc_id == crtc_id) score += 15;
    return score;
}

static int overlay_score(const plane_candidate_t *candidate, uint32_t crtc_id)
{
    (void)crtc_id;
    if (!candidate->plane.supports_argb) return -1;
    if (candidate->type_known && candidate->plane.type != OCR_PLANE_OVERLAY) return -1;
    /* 只借用空闲 overlay，绝不抢占当前 CRTC 或其他显示输出。 */
    if (candidate->plane.crtc_id != 0 || candidate->plane.fb_id != 0) return -1;

    int score = 20;
    if (candidate->type_known && candidate->plane.type == OCR_PLANE_OVERLAY) score += 120;
    if (candidate->plane.blend_prop_id != 0) score += 10;
    return score;
}

static void try_set_property(ocr_drm_device_t *dev, ocr_drm_plane_t *plane,
                             uint32_t prop_id, uint64_t value,
                             const char *name)
{
    if (!dev || !plane || prop_id == 0) return;
    if (drmModeObjectSetProperty(dev->fd, plane->plane_id,
                                 DRM_MODE_OBJECT_PLANE,
                                 prop_id, value) != 0) {
        LOG_W("Plane %u 设置 %s=%llu 失败（可能为 immutable）: %s",
              plane->plane_id, name, (unsigned long long)value, strerror(errno));
    }
}

int ocr_drm_planes_init(ocr_drm_planes_t *planes, ocr_drm_device_t *dev)
{
    if (!planes || !dev || !dev->opened || dev->fd < 0 ||
        !dev->crtc || dev->crtc_index < 0) {
        return -1;
    }
    memset(planes, 0, sizeof(*planes));
    planes->dev = dev;

    drmModePlaneRes *plane_res = drmModeGetPlaneResources(dev->fd);
    if (!plane_res) {
        LOG_E("drmModeGetPlaneResources 失败: %s", strerror(errno));
        return -2;
    }

    plane_candidate_t *candidates = (plane_candidate_t *)
        calloc(plane_res->count_planes, sizeof(*candidates));
    if (!candidates) {
        drmModeFreePlaneResources(plane_res);
        return -3;
    }

    int candidate_count = 0;
    for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(dev->fd, plane_res->planes[i]);
        if (!p) continue;

        if (dev->crtc_index >= 32 ||
            !(p->possible_crtcs & (1U << dev->crtc_index))) {
            drmModeFreePlane(p);
            continue;
        }
        if (p->crtc_id != 0 && p->crtc_id != dev->crtc_id) {
            drmModeFreePlane(p);
            continue;
        }

        plane_candidate_t *candidate = &candidates[candidate_count++];
        candidate->plane.plane_id = p->plane_id;
        candidate->plane.crtc_id = p->crtc_id;
        candidate->plane.fb_id = p->fb_id;
        candidate->plane.possible_crtcs = p->possible_crtcs;
        candidate->plane.supports_nv12 = plane_has_format(p, DRM_FORMAT_NV12);
        candidate->plane.supports_argb = plane_has_format(p, DRM_FORMAT_ARGB8888);
        read_plane_properties(dev->fd, p->plane_id, candidate);
        drmModeFreePlane(p);
    }

    /* 联合选择两个不同的 plane，避免先选的视频 plane 抢占唯一 ARGB plane。 */
    int best_base = -1;
    int best_overlay = -1;
    int best_pair_score = -1;
    for (int i = 0; i < candidate_count; ++i) {
        int bs = base_score(&candidates[i], dev->crtc_id);
        if (bs < 0) continue;
        for (int j = 0; j < candidate_count; ++j) {
            if (i == j) continue;
            int os = overlay_score(&candidates[j], dev->crtc_id);
            if (os < 0) continue;
            if (bs + os > best_pair_score) {
                best_pair_score = bs + os;
                best_base = i;
                best_overlay = j;
            }
        }
    }

    if (best_base < 0) {
        /* Overlay 是可选能力；没有配对时仍应选择最合适的 NV12 primary。 */
        int best_score = -1;
        for (int i = 0; i < candidate_count; ++i) {
            int score = base_score(&candidates[i], dev->crtc_id);
            if (score > best_score) {
                best_score = score;
                best_base = i;
            }
        }
    }

    if (best_base < 0) {
        LOG_E("找不到满足 CRTC%u + NV12 的 Primary Plane", dev->crtc_id);
        for (int i = 0; i < candidate_count; ++i) {
            LOG_I("  Plane %u: type=%d nv12=%d argb=%d possible_crtcs=0x%x",
                  candidates[i].plane.plane_id, candidates[i].plane.type,
                  candidates[i].plane.supports_nv12,
                  candidates[i].plane.supports_argb,
                  candidates[i].plane.possible_crtcs);
        }
        free(candidates);
        drmModeFreePlaneResources(plane_res);
        memset(planes, 0, sizeof(*planes));
        return -4;
    }

    planes->planes[0] = candidates[best_base].plane;
    planes->plane_count = 1;
    planes->primary = &planes->planes[0];
    if (best_overlay >= 0) {
        planes->planes[1] = candidates[best_overlay].plane;
        planes->overlay = &planes->planes[1];
        planes->plane_count = 2;
    }
    planes->primary->original_crtc_id = planes->primary->crtc_id;
    planes->primary->original_fb_id = planes->primary->fb_id;
    planes->primary->original_zpos = planes->primary->zpos;
    planes->primary->original_blend_value = planes->primary->blend_value;
    planes->primary->crtc_id = dev->crtc_id;
    planes->primary->fb_id = 0;
    planes->primary->in_use = 0;
    if (planes->overlay) {
        planes->overlay->original_crtc_id = planes->overlay->crtc_id;
        planes->overlay->original_fb_id = planes->overlay->fb_id;
        planes->overlay->original_zpos = planes->overlay->zpos;
        planes->overlay->original_blend_value = planes->overlay->blend_value;
        planes->overlay->crtc_id = dev->crtc_id;
        planes->overlay->fb_id = 0;
        planes->overlay->in_use = 0;
    }

    /* zpos / blend mode 是驱动属性：支持时设置，失败不掩盖格式能力结果。 */
    try_set_property(dev, planes->primary, planes->primary->zpos_prop_id,
                     planes->primary->zpos_min, "zpos");
    if (planes->overlay) {
        try_set_property(dev, planes->overlay, planes->overlay->zpos_prop_id,
                         planes->overlay->zpos_max, "zpos");
        try_set_property(dev, planes->overlay, planes->overlay->blend_prop_id,
                         planes->overlay->premult_value, "pixel blend mode");
    }

    if (planes->overlay) {
        LOG_I("DRM Plane: base=%u(NV12,type=%d) overlay=%u(ARGB8888,type=%d)",
              planes->primary->plane_id, planes->primary->type,
              planes->overlay->plane_id, planes->overlay->type);
    } else {
        LOG_W("DRM Plane: base=%u 可用，但没有空闲 ARGB8888 overlay",
              planes->primary->plane_id);
    }

    free(candidates);
    drmModeFreePlaneResources(plane_res);
    return 0;
}

int ocr_drm_plane_set_fb_ex(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                            uint32_t fb_id,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t src_w, uint32_t src_h,
                            uint32_t dst_x, uint32_t dst_y,
                            uint32_t dst_w, uint32_t dst_h)
{
    if (!planes || !plane || !planes->dev || !planes->dev->opened ||
        planes->dev->fd < 0 || fb_id == 0 ||
        src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return -1;
    }
    if (src_x > UINT16_MAX || src_y > UINT16_MAX ||
        src_w > UINT16_MAX || src_h > UINT16_MAX ||
        dst_x > INT32_MAX || dst_y > INT32_MAX) {
        return -2;
    }

    ocr_drm_device_t *dev = planes->dev;
    if (!dev->mode_set) {
        if (plane != planes->primary) {
            LOG_E("必须先在底图 Plane 上完成首帧 modeset");
            return -3;
        }
        if (src_x != 0 || src_y != 0 || src_w != dev->width || src_h != dev->height ||
            dst_x != 0 || dst_y != 0 || dst_w != dev->width || dst_h != dev->height) {
            LOG_E("首帧 legacy modeset 不支持缩放；请先用 RGA 输出 %ux%u",
                  dev->width, dev->height);
            return -4;
        }
        if (ocr_drm_modeset(dev, fb_id) != 0) return -5;
        /* drmModeSetCrtc 已将 fb 绑定到 Primary plane，避免重复 SetPlane。 */
        plane->fb_id = fb_id;
        plane->crtc_id = dev->crtc_id;
        plane->in_use = 1;
        return 0;
    }

    int ret = drmModeSetPlane(dev->fd, plane->plane_id, dev->crtc_id,
                              fb_id, 0,
                              (int32_t)dst_x, (int32_t)dst_y,
                              dst_w, dst_h,
                              src_x << 16, src_y << 16,
                              src_w << 16, src_h << 16);
    if (ret != 0) {
        LOG_E("drmModeSetPlane plane=%u fb=%u 失败: %s",
              plane->plane_id, fb_id, strerror(errno));
        return -6;
    }
    plane->fb_id = fb_id;
    plane->crtc_id = dev->crtc_id;
    plane->in_use = 1;
    return 0;
}

int ocr_drm_plane_set_fb(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                         uint32_t fb_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h)
{
    return ocr_drm_plane_set_fb_ex(planes, plane, fb_id,
                                   0, 0, w, h, x, y, w, h);
}

int ocr_drm_plane_disable(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane)
{
    if (!planes || !plane || !planes->dev || !planes->dev->opened ||
        planes->dev->fd < 0) return -1;
    if (!plane->in_use && plane->fb_id == 0) return 0;

    int ret;
    if (plane == planes->primary) {
        /* Primary plane 通过 CRTC modeset 绑定，使用 SetCrtc 可靠解除 scanout。 */
        ret = drmModeSetCrtc(planes->dev->fd, planes->dev->crtc_id,
                             0, 0, 0, NULL, 0, NULL);
    } else {
        ret = drmModeSetPlane(planes->dev->fd, plane->plane_id, 0, 0, 0,
                              0, 0, 0, 0, 0, 0, 0, 0);
    }
    if (ret != 0) {
        LOG_W("禁用 Plane %u 失败: %s", plane->plane_id, strerror(errno));
        return -2;
    }
    plane->fb_id = 0;
    plane->in_use = 0;
    if (plane == planes->primary) planes->dev->mode_set = 0;
    return 0;
}

void ocr_drm_planes_destroy(ocr_drm_planes_t *planes)
{
    if (!planes) return;
    /* 只禁用本模块实际选中并启用的 plane，绝不触碰未选中的系统 plane。 */
    if (planes->overlay) (void)ocr_drm_plane_disable(planes, planes->overlay);
    if (planes->primary) (void)ocr_drm_plane_disable(planes, planes->primary);
    if (planes->dev && planes->dev->opened) {
        if (planes->primary) {
            try_set_property(planes->dev, planes->primary,
                             planes->primary->zpos_prop_id,
                             planes->primary->original_zpos, "zpos restore");
            try_set_property(planes->dev, planes->primary,
                             planes->primary->blend_prop_id,
                             planes->primary->original_blend_value,
                             "pixel blend mode restore");
        }
        if (planes->overlay) {
            try_set_property(planes->dev, planes->overlay,
                             planes->overlay->zpos_prop_id,
                             planes->overlay->original_zpos, "zpos restore");
            try_set_property(planes->dev, planes->overlay,
                             planes->overlay->blend_prop_id,
                             planes->overlay->original_blend_value,
                             "pixel blend mode restore");
        }
    }
    memset(planes, 0, sizeof(*planes));
}
