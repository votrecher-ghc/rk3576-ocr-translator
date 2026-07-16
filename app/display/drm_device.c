/**
 * @file drm_device.c
 * @brief DRM 设备、connector、encoder 与 CRTC 选择
 */
#include "drm_device.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* 判断某个 CRTC 是否已被另一个已连接 connector 使用。 */
static int crtc_used_by_other_connector(const ocr_drm_device_t *dev,
                                        uint32_t crtc_id,
                                        uint32_t connector_id)
{
    if (!dev || !dev->res) return 0;

    for (int i = 0; i < dev->res->count_connectors; ++i) {
        uint32_t id = dev->res->connectors[i];
        if (id == connector_id) continue;

        drmModeConnector *conn = drmModeGetConnector(dev->fd, id);
        if (!conn) continue;

        int used = 0;
        if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id != 0) {
            drmModeEncoder *enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
            if (enc) {
                used = enc->crtc_id == crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
        if (used) return 1;
    }
    return 0;
}

/*
 * 为 connector 选择 encoder/CRTC。优先保持固件当前路由，其次选择未被
 * 其他 connector 占用的 possible_crtcs。不会偷用正在驱动其他屏幕的 CRTC。
 */
static int choose_crtc_for_connector(const ocr_drm_device_t *dev,
                                     const drmModeConnector *conn,
                                     uint32_t *crtc_id,
                                     int *crtc_index)
{
    if (!dev || !dev->res || !conn || !crtc_id || !crtc_index) return -1;

    /* 已激活 connector 必须沿用其当前 CRTC，不能破坏克隆/多屏拓扑。 */
    if (conn->encoder_id != 0) {
        drmModeEncoder *current = drmModeGetEncoder(dev->fd, conn->encoder_id);
        if (current) {
            if (current->crtc_id != 0) {
                for (int i = 0; i < dev->res->count_crtcs && i < 32; ++i) {
                    if (dev->res->crtcs[i] == current->crtc_id &&
                        (current->possible_crtcs & (1U << i))) {
                        *crtc_id = current->crtc_id;
                        *crtc_index = i;
                        drmModeFreeEncoder(current);
                        return 0;
                    }
                }
                drmModeFreeEncoder(current);
                return -1;
            }
            drmModeFreeEncoder(current);
        }
    }

    int best_score = -1;
    uint32_t best_id = 0;
    int best_index = -1;

    /* current encoder 放在逻辑上的第一位，但仍扫描全部 encoder。 */
    for (int pass = -1; pass < conn->count_encoders; ++pass) {
        uint32_t encoder_id = (pass < 0) ? conn->encoder_id : conn->encoders[pass];
        if (encoder_id == 0) continue;

        /* 避免 current encoder 在 encoders[] 中重复计分。 */
        if (pass >= 0 && encoder_id == conn->encoder_id) continue;

        drmModeEncoder *enc = drmModeGetEncoder(dev->fd, encoder_id);
        if (!enc) continue;

        for (int i = 0; i < dev->res->count_crtcs && i < 32; ++i) {
            if (!(enc->possible_crtcs & (1U << i))) continue;

            uint32_t candidate = dev->res->crtcs[i];
            if (crtc_used_by_other_connector(dev, candidate, conn->connector_id)) {
                continue;
            }

            int score = 10;
            if (enc->crtc_id == candidate) score += 100;
            if (encoder_id == conn->encoder_id) score += 20;

            if (score > best_score) {
                best_score = score;
                best_id = candidate;
                best_index = i;
            }
        }
        drmModeFreeEncoder(enc);
    }

    if (best_index < 0) return -1;
    *crtc_id = best_id;
    *crtc_index = best_index;
    return 0;
}

static int collect_routed_connectors(ocr_drm_device_t *dev)
{
    int count = 0;
    for (int i = 0; i < dev->res->count_connectors; ++i) {
        uint32_t id = dev->res->connectors[i];
        drmModeConnector *conn = drmModeGetConnector(dev->fd, id);
        if (!conn) continue;

        int routed = 0;
        if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id != 0) {
            drmModeEncoder *enc = drmModeGetEncoder(dev->fd, conn->encoder_id);
            if (enc) {
                routed = enc->crtc_id == dev->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
        if (!routed) continue;
        if (count >= OCR_DRM_MAX_CONNECTORS) return -1;
        dev->routed_connectors[count++] = id;
    }

    if (count == 0) {
        dev->routed_connectors[count++] = dev->connector_id;
    }
    dev->routed_connector_count = count;
    return 0;
}

static int choose_mode(const drmModeConnector *conn,
                       uint32_t width, uint32_t height,
                       const drmModeModeInfo *current,
                       drmModeModeInfo *selected)
{
    if (!conn || !selected || conn->count_modes <= 0) return -1;
    if (current && current->hdisplay == width && current->vdisplay == height) {
        *selected = *current;
        return 0;
    }
    int best = -1;
    for (int i = 0; i < conn->count_modes; ++i) {
        if (width && height &&
            (conn->modes[i].hdisplay != width || conn->modes[i].vdisplay != height))
            continue;
        if (best < 0 ||
            ((conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) &&
             !(conn->modes[best].type & DRM_MODE_TYPE_PREFERRED)) ||
            (conn->modes[i].vrefresh > conn->modes[best].vrefresh &&
             ((conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) ==
              (conn->modes[best].type & DRM_MODE_TYPE_PREFERRED))))
            best = i;
    }
    if (best < 0) return -1;
    *selected = conn->modes[best];
    return 0;
}

int ocr_drm_open(ocr_drm_device_t *dev, const char *dev_path)
{
    if (!dev || !dev_path || dev_path[0] == '\0') return -1;

    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    dev->crtc_index = -1;

    dev->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        LOG_E("打开 DRM 设备 %s 失败: %s", dev_path, strerror(errno));
        return -2;
    }
    dev->opened = 1;

    if (!drmIsKMS(dev->fd)) {
        LOG_E("DRM 设备 %s 不支持 KMS", dev_path);
        ocr_drm_close(dev);
        return -3;
    }

    /* Primary/Cursor plane 默认可能被隐藏；枚举前先请求 universal planes。 */
    if (drmSetClientCap(dev->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) == 0) {
        dev->universal_planes = 1;
    } else {
        LOG_W("DRM_CLIENT_CAP_UNIVERSAL_PLANES 不可用: %s", strerror(errno));
    }

    dev->res = drmModeGetResources(dev->fd);
    if (!dev->res) {
        LOG_E("drmModeGetResources 失败: %s", strerror(errno));
        ocr_drm_close(dev);
        return -4;
    }

    LOG_I("DRM 设备已打开: %s (connectors=%d crtcs=%d)",
          dev_path, dev->res->count_connectors, dev->res->count_crtcs);
    return 0;
}

int ocr_drm_setup_crtc_mode(ocr_drm_device_t *dev,
                            uint32_t desired_width, uint32_t desired_height)
{
    if (!dev || !dev->opened || dev->fd < 0 || !dev->res) return -1;
    if (dev->mode_set || dev->restore_needed) {
        LOG_E("显示模式已激活，不能重新选择 CRTC");
        return -2;
    }

    if (dev->crtc) {
        drmModeFreeCrtc(dev->crtc);
        dev->crtc = NULL;
    }
    if (dev->connector) {
        drmModeFreeConnector(dev->connector);
        dev->connector = NULL;
    }
    dev->connector_id = 0;
    dev->crtc_id = 0;
    dev->crtc_index = -1;
    dev->original_connector_crtc_id = 0;
    dev->routed_connector_count = 0;
    memset(dev->routed_connectors, 0, sizeof(dev->routed_connectors));

    /* 不假定第一个 connected connector 一定具有可用 encoder/CRTC。 */
    for (int i = 0; i < dev->res->count_connectors; ++i) {
        drmModeConnector *conn =
            drmModeGetConnector(dev->fd, dev->res->connectors[i]);
        if (!conn) continue;

        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes <= 0) {
            drmModeFreeConnector(conn);
            continue;
        }

        uint32_t crtc_id = 0;
        int crtc_index = -1;
        if (choose_crtc_for_connector(dev, conn, &crtc_id, &crtc_index) != 0) {
            drmModeFreeConnector(conn);
            continue;
        }

        drmModeCrtc *crtc = drmModeGetCrtc(dev->fd, crtc_id);
        if (!crtc) {
            drmModeFreeConnector(conn);
            continue;
        }

        dev->connector = conn;
        dev->crtc = crtc;
        dev->connector_id = conn->connector_id;
        dev->crtc_id = crtc_id;
        dev->crtc_index = crtc_index;
        if (conn->encoder_id != 0) {
            drmModeEncoder *original = drmModeGetEncoder(dev->fd, conn->encoder_id);
            if (original) {
                dev->original_connector_crtc_id = original->crtc_id;
                drmModeFreeEncoder(original);
            }
        }
        const drmModeModeInfo *current_mode =
            (dev->original_connector_crtc_id == crtc_id && crtc->mode_valid)
            ? &crtc->mode : NULL;
        if (choose_mode(conn, desired_width, desired_height,
                        current_mode, &dev->mode) != 0) {
            drmModeFreeCrtc(dev->crtc);
            drmModeFreeConnector(dev->connector);
            dev->crtc = NULL;
            dev->connector = NULL;
            dev->connector_id = 0;
            dev->crtc_id = 0;
            dev->crtc_index = -1;
            dev->original_connector_crtc_id = 0;
            continue;
        }
        dev->width = dev->mode.hdisplay;
        dev->height = dev->mode.vdisplay;
        dev->refresh = (uint32_t)dev->mode.vrefresh * 1000U;
        if (collect_routed_connectors(dev) != 0) {
            drmModeFreeCrtc(dev->crtc);
            drmModeFreeConnector(dev->connector);
            dev->crtc = NULL;
            dev->connector = NULL;
            dev->connector_id = 0;
            dev->crtc_id = 0;
            dev->crtc_index = -1;
            dev->original_connector_crtc_id = 0;
            dev->routed_connector_count = 0;
            memset(dev->routed_connectors, 0,
                   sizeof(dev->routed_connectors));
            continue;
        }
        break;
    }

    if (!dev->connector || !dev->crtc) {
        LOG_E("未找到同时具备有效 mode、encoder 和空闲 CRTC 的 connector");
        return -3;
    }

    LOG_I("DRM 路由: connector=%u crtc=%u(index=%d) mode=%s %ux%u@%umHz",
          dev->connector_id, dev->crtc_id, dev->crtc_index, dev->mode.name,
          dev->width, dev->height, dev->refresh);
    return 0;
}

int ocr_drm_setup_crtc(ocr_drm_device_t *dev)
{
    return ocr_drm_setup_crtc_mode(dev, 0, 0);
}

int ocr_drm_modeset(ocr_drm_device_t *dev, uint32_t fb_id)
{
    if (!dev || !dev->opened || dev->fd < 0 || !dev->connector ||
        !dev->crtc || fb_id == 0) {
        return -1;
    }

    if (drmModeSetCrtc(dev->fd, dev->crtc_id, fb_id, 0, 0,
                       dev->routed_connectors,
                       dev->routed_connector_count,
                       &dev->mode) != 0) {
        LOG_E("drmModeSetCrtc connector=%u crtc=%u fb=%u 失败: %s",
              dev->connector_id, dev->crtc_id, fb_id, strerror(errno));
        return -2;
    }
    dev->mode_set = 1;
    dev->restore_needed = 1;
    return 0;
}

void ocr_drm_close(ocr_drm_device_t *dev)
{
    if (!dev) return;

    /* 允许对零初始化/从未 open 的全局上下文安全调用 destroy。 */
    if (!dev->opened) {
        memset(dev, 0, sizeof(*dev));
        dev->fd = -1;
        dev->crtc_index = -1;
        return;
    }

    /* 尽力恢复打开设备时的 CRTC 状态，避免退出后留下黑屏。 */
    if (dev->fd >= 0 && dev->restore_needed && dev->crtc) {
        int ret;
        if (dev->crtc->mode_valid &&
            dev->original_connector_crtc_id == dev->crtc->crtc_id) {
            ret = drmModeSetCrtc(dev->fd, dev->crtc->crtc_id,
                                 dev->crtc->buffer_id,
                                 dev->crtc->x, dev->crtc->y,
                                 dev->routed_connectors,
                                 dev->routed_connector_count,
                                 &dev->crtc->mode);
        } else {
            ret = drmModeSetCrtc(dev->fd, dev->crtc_id, 0, 0, 0,
                                 NULL, 0, NULL);
        }
        if (ret != 0) {
            LOG_W("恢复原 CRTC 状态失败: %s", strerror(errno));
        }
    }

    if (dev->crtc) {
        drmModeFreeCrtc(dev->crtc);
        dev->crtc = NULL;
    }
    if (dev->connector) {
        drmModeFreeConnector(dev->connector);
        dev->connector = NULL;
    }
    if (dev->res) {
        drmModeFreeResources(dev->res);
        dev->res = NULL;
    }
    if (dev->fd >= 0) close(dev->fd);

    memset(dev, 0, sizeof(*dev));
    dev->fd = -1;
    dev->crtc_index = -1;
}
