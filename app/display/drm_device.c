/**
 * @file drm_device.c
 * @brief DRM 设备管理实现
 */
#include "drm_device.h"
#include "log.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int ocr_drm_open(ocr_drm_device_t *dev, const char *dev_path)
{
    if (!dev || !dev_path) return -1;
    memset(dev, 0, sizeof(*dev));

    dev->fd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (dev->fd < 0) {
        LOG_E("打开 DRM 设备 %s 失败: %s", dev_path, strerror(errno));
        return -2;
    }

    /* 检查是否支持通用 DRM（KMS） */
    if (!drmIsKMS(dev->fd)) {
        LOG_E("DRM 设备 %s 不支持 KMS", dev_path);
        close(dev->fd);
        dev->fd = -1;
        return -3;
    }

    dev->res = drmModeGetResources(dev->fd);
    if (!dev->res) {
        LOG_E("drmModeGetResources 失败: %s", strerror(errno));
        close(dev->fd);
        dev->fd = -1;
        return -4;
    }

    LOG_I("DRM 设备已打开: %s (count_connectors=%d count_crtcs=%d)",
          dev_path, dev->res->count_connectors, dev->res->count_crtcs);
    return 0;
}

int ocr_drm_setup_crtc(ocr_drm_device_t *dev)
{
    if (!dev || dev->fd < 0 || !dev->res) return -1;

    /* 查找已连接的 connector */
    for (int i = 0; i < dev->res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(dev->fd, dev->res->connectors[i]);
        if (!conn) continue;
        if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
            dev->connector = conn;
            break;
        }
        drmModeFreeConnector(conn);
    }

    if (!dev->connector) {
        LOG_E("未找到已连接的 DRM connector");
        return -2;
    }

    /* 选择第一个模式（通常是首选分辨率） */
    drmModeModeInfo *mode = &dev->connector->modes[0];
    dev->width = mode->hdisplay;
    dev->height = mode->vdisplay;
    dev->refresh = mode->vrefresh * 1000;

    /* 查找可用 CRTC */
    for (int i = 0; i < dev->res->count_crtcs; i++) {
        if (dev->connector->encoder_id) {
            drmModeEncoder *enc = drmModeGetEncoder(dev->fd, dev->connector->encoder_id);
            if (enc) {
                if (enc->possible_crtcs & (1 << i)) {
                    dev->crtc = drmModeGetCrtc(dev->fd, dev->res->crtcs[i]);
                    drmModeFreeEncoder(enc);
                    break;
                }
                drmModeFreeEncoder(enc);
            }
        }
    }

    if (!dev->crtc) {
        LOG_E("未找到可用 CRTC");
        return -3;
    }

    LOG_I("DRM CRTC 已设置: %ux%u@%umHz", dev->width, dev->height, dev->refresh);
    return 0;
}

void ocr_drm_close(ocr_drm_device_t *dev)
{
    if (!dev) return;
    if (dev->crtc) { drmModeFreeCrtc(dev->crtc); dev->crtc = NULL; }
    if (dev->connector) { drmModeFreeConnector(dev->connector); dev->connector = NULL; }
    if (dev->res) { drmModeFreeResources(dev->res); dev->res = NULL; }
    if (dev->fd >= 0) { close(dev->fd); dev->fd = -1; }
}
