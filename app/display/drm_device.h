#ifndef OCR_DISPLAY_DRM_DEVICE_H
#define OCR_DISPLAY_DRM_DEVICE_H
/**
 * @file drm_device.h
 * @brief DRM 设备管理：open/resources/crtc/connector
 *
 * DRM 设备与显示模式管理：发现已连接 connector、匹配 encoder/CRTC，并保存原有显示状态。
 * 首帧通过 modeset 建立扫描输出；退出时尽可能恢复原显示配置，再释放查询得到的 DRM 对象。
 */

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define OCR_DRM_MAX_CONNECTORS 32

/** DRM 设备上下文 */
typedef struct {
    int              fd; /**< 本设备拥有的 DRM/KMS fd，open 创建、close 关闭；依赖它的 FB/plane 须先退出。 */
    drmModeRes      *res; /**< drmModeGetResources 返回的资源快照，本模块负责 drmModeFreeResources。 */
    drmModeConnector *connector; /**< 模式协商选中的已连接 connector 查询对象，本模块负责释放。 */
    drmModeCrtc     *crtc; /**< 选定 CRTC 在接管前的状态快照，供 close 恢复，不是每帧实时查询结果。 */
    drmModeModeInfo  mode; /**< 选择的显示时序值副本，首帧 modeset 使用；包含尺寸及刷新时序。 */
    uint32_t         connector_id; /**< 所选显示连接器的 KMS 对象 ID，不是资源数组下标。 */
    uint32_t         crtc_id; /**< 所选扫描控制器 KMS 对象 ID，用于 modeset/pageflip/plane 绑定。 */
    uint32_t         original_connector_crtc_id; /**< 接管前当前 connector 的路由 CRTC ID，用于判断和恢复原路由。 */
    uint32_t         routed_connectors[OCR_DRM_MAX_CONNECTORS]; /**< 恢复/设置所选 CRTC 时使用的 connector ID 列表，前 routed_connector_count 项有效。 */
    int              routed_connector_count; /**< routed_connectors 有效项数，不是系统所有 connector 数。 */
    int              crtc_index; /**< crtc_id 在 res->crtcs 中的下标，用来解释 possible_crtcs 位图及 vblank 选择。 */
    uint32_t         width; /**< 最终显示模式宽度，像素；输出池按此尺寸分配，不一定等于配置请求。 */
    uint32_t         height; /**< 最终显示模式高度，像素；primary framebuffer 必须与其匹配。 */
    uint32_t         refresh; /**< 记录的模式刷新率，毫赫兹；代码取 mode.vrefresh*1000。 */
    int              mode_set; /**< 本模块已激活所选 CRTC 扫描时为 1；决定首帧 modeset 或后续 pageflip。 */
    int              restore_needed; /**< 接管后需要在 close 恢复原显示状态的标记，与当前扫描是否启用区分。 */
    int              universal_planes; /**< 成功请求 universal planes 的标记，决定能否统一枚举 primary/overlay 等层。 */
    int              opened; /**< DRM fd 生命周期建立标志；不表示模式已选择或屏幕已开始显示。 */
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
 * @brief 寻找同时具有可用显示模式和兼容 CRTC 的已连接 connector。
 */
int ocr_drm_setup_crtc(ocr_drm_device_t *dev);

/** Select a connector/CRTC mode matching the configured dimensions.
 *
 * 根据目标分辨率选择显示模式，为首帧 modeset 准备 CRTC。
 */
int ocr_drm_setup_crtc_mode(ocr_drm_device_t *dev,
                            uint32_t width, uint32_t height);

/**
 * @brief 使用指定 framebuffer 激活选中的 connector/CRTC/mode
 *
 * 首帧显示必须先完成 modeset；后续帧可使用 page flip。
 */
int ocr_drm_modeset(ocr_drm_device_t *dev, uint32_t fb_id);

#endif /* OCR_DISPLAY_DRM_DEVICE_H */
