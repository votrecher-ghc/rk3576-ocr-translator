#ifndef OCR_DISPLAY_DRM_DEVICE_H
#define OCR_DISPLAY_DRM_DEVICE_H
/**
 * @file drm_device.h
 * @brief DRM 设备管理：open/resources/crtc/connector
 */

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

/** DRM 设备上下文 */
typedef struct {
    int              fd;          /* DRM 设备 fd */
    drmModeRes      *res;         /* DRM 资源 */
    drmModeConnector *connector;  /* 当前连接器 */
    drmModeCrtc     *crtc;        /* 当前 CRTC */
    uint32_t         width;       /* 显示宽度 */
    uint32_t         height;      /* 显示高度 */
    uint32_t         refresh;     /* 刷新率（mHz） */
} ocr_drm_device_t;

/**
 * @brief 打开 DRM 设备
 * @param[in] dev      设备上下文
 * @param[in] dev_path 设备路径（如 /dev/dri/card0）
 * @return 0=成功，负数=错误
 */
int ocr_drm_open(ocr_drm_device_t *dev, const char *dev_path);

/**
 * @brief 关闭 DRM 设备
 */
void ocr_drm_close(ocr_drm_device_t *dev);

/**
 * @brief 获取 CRTC 与连接器（选择已连接的第一个 connector）
 */
int ocr_drm_setup_crtc(ocr_drm_device_t *dev);

#endif /* OCR_DISPLAY_DRM_DEVICE_H */
