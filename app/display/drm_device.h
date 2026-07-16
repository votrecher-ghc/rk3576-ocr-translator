#ifndef OCR_DISPLAY_DRM_DEVICE_H
#define OCR_DISPLAY_DRM_DEVICE_H
/**
 * @file drm_device.h
 * @brief DRM 设备管理：open/resources/crtc/connector
 */

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define OCR_DRM_MAX_CONNECTORS 32

/** DRM 设备上下文 */
typedef struct {
    int              fd;          /* DRM 设备 fd */
    drmModeRes      *res;         /* DRM 资源 */
    drmModeConnector *connector;  /* 当前连接器 */
    drmModeCrtc     *crtc;        /* 打开时的 CRTC 状态（关闭时用于恢复） */
    drmModeModeInfo  mode;        /* 选中的显示模式 */
    uint32_t         connector_id;/* 选中的 connector ID */
    uint32_t         crtc_id;     /* 选中的 CRTC ID */
    uint32_t         original_connector_crtc_id; /* connector 打开前所连 CRTC */
    uint32_t         routed_connectors[OCR_DRM_MAX_CONNECTORS];
    int              routed_connector_count; /* 打开时路由到选中 CRTC 的 connector */
    int              crtc_index;  /* CRTC 在 drmModeRes.crtcs 中的索引 */
    uint32_t         width;       /* 显示宽度 */
    uint32_t         height;      /* 显示高度 */
    uint32_t         refresh;     /* 刷新率（mHz） */
    int              mode_set;    /* 选中 CRTC 当前是否处于 active scanout */
    int              restore_needed; /* 关闭时是否需要恢复打开前状态 */
    int              universal_planes; /* 是否成功启用 universal planes */
    int              opened;      /* 设备 fd/资源生命周期已建立 */
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

/** Select a connector/CRTC mode matching the configured dimensions. */
int ocr_drm_setup_crtc_mode(ocr_drm_device_t *dev,
                            uint32_t width, uint32_t height);

/**
 * @brief 使用指定 framebuffer 激活选中的 connector/CRTC/mode
 *
 * 首帧显示必须先完成 modeset；后续帧可使用 page flip。
 */
int ocr_drm_modeset(ocr_drm_device_t *dev, uint32_t fb_id);

#endif /* OCR_DISPLAY_DRM_DEVICE_H */
