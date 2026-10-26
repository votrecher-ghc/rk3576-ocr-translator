#ifndef OCR_DISPLAY_DRM_PLANE_H
#define OCR_DISPLAY_DRM_PLANE_H
/**
 * @file drm_plane.h
 * @brief Plane 管理：Plane0 底图 / Plane1 叠加
 *
 * 按 CRTC 兼容性和像素格式选择 NV12 primary 及可选 ARGB overlay，并设置显示区域。
 * 源坐标使用 KMS 的 16.16 格式，目标坐标使用屏幕像素；退出时只处理本模块选中的 plane。
 */

#include "drm_device.h"

#define DRM_MAX_PLANES 4

/** Plane 类型 */
typedef enum {
    OCR_PLANE_OVERLAY = 0,
    OCR_PLANE_PRIMARY,
    OCR_PLANE_CURSOR,
} ocr_plane_type_t;

/** Plane 上下文 */
typedef struct {
    uint32_t plane_id; /**< DRM plane 对象 ID，驱动分配；primary/overlay 不是固定 ID 0/1。 */
    uint32_t crtc_id; /**< 该 plane 当前记录的目标 CRTC ID；退出时与 original_crtc_id 区分。 */
    ocr_plane_type_t type; /**< KMS plane 类型枚举：primary、overlay 或 cursor，不代表数组顺序。 */
    uint32_t fb_id; /**< 本模块记录的当前绑定 framebuffer ID，0 表示无绑定。 */
    int      in_use; /**< 本模块已把图像绑定到此 plane 的标记，用于只清理实际使用的层。 */
    uint32_t possible_crtcs; /**< 兼容 CRTC 的位图，位号是资源数组索引 crtc_index，不是 crtc_id。 */
    int      supports_nv12; /**< 驱动枚举格式中包含 DRM_FORMAT_NV12 时为 1，用于底图筛选。 */
    int      supports_argb; /**< 包含 DRM_FORMAT_ARGB8888 时为 1，用于文字层筛选。 */
    uint32_t zpos_prop_id; /**< zpos 层叠顺序属性的 DRM ID；0 表示未找到，不能据此直接设置。 */
    uint64_t zpos_min; /**< 查询所得 zpos 可用下界，底图尽量设到此层；不可查询范围时退化为原值。 */
    uint64_t zpos_max; /**< 查询所得 zpos 上界，文字层尽量设到此层。 */
    uint64_t zpos; /**< 枚举时的 zpos 属性值快照；设置后未持续刷新，不是实时查询值。 */
    uint32_t blend_prop_id; /**< 支持预乘混合模式时记录的 pixel blend mode 属性 ID，否则为 0。 */
    uint64_t premult_value; /**< 驱动 Pre-multiplied 枚举值，不能假定是固定整数；文字像素按预乘方式生成。 */
    uint32_t original_crtc_id; /**< 选中前绑定的 CRTC ID 快照，用于记录接管前状态。 */
    uint32_t original_fb_id; /**< 选中前绑定的 framebuffer ID 快照，不表示本模块拥有该 FB。 */
    uint64_t original_zpos; /**< 接管前 zpos，planes_destroy 尝试恢复该属性。 */
    uint64_t original_blend_value; /**< 接管前混合模式属性值，退出时在支持对应属性时尝试恢复。 */
    uint64_t blend_value; /**< 枚举时读取的混合模式值快照，供保存 original_blend_value 使用。 */
} ocr_drm_plane_t;

/** Plane 管理器 */
typedef struct {
    ocr_drm_device_t *dev; /**< 借用的 DRM 设备，所有 plane 操作使用其 fd/CRTC；须比管理器活得久。 */
    ocr_drm_plane_t   planes[DRM_MAX_PLANES]; /**< 本管理器内嵌的选中 plane 元数据数组，不是所有硬件 plane 的完整列表。 */
    int               plane_count; /**< 选中数组有效项数，通常 1 个 primary 加可选 1 个 overlay。 */
    ocr_drm_plane_t  *primary; /**< 指向 planes 内选中的 NV12 底图层；不是独立分配对象，不单独 free。 */
    ocr_drm_plane_t  *overlay; /**< 指向 planes 内选中的空闲 ARGB 文字层，无可用层时为 NULL。 */
} ocr_drm_planes_t;

/**
 * @brief 枚举所有 Plane 并分类
 */
int ocr_drm_planes_init(ocr_drm_planes_t *planes, ocr_drm_device_t *dev);

/**
 * @brief 设置 Plane 的 framebuffer 与位置
 * @param[in] planes  Plane 管理器
 * @param[in] plane   目标 Plane
 * @param[in] fb_id   framebuffer ID
 * @param[in] x,y     显示位置
 * @param[in] w,h     显示宽高
 * @return 0=成功，负数=错误
 */
int ocr_drm_plane_set_fb(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                         uint32_t fb_id, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h);

/**
 * @brief 设置 Plane framebuffer，并分别指定源、目标矩形
 *
 * src 使用像素单位，内部转换为 DRM 16.16 固定点；dst 为屏幕坐标。
 */
int ocr_drm_plane_set_fb_ex(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane,
                            uint32_t fb_id,
                            uint32_t src_x, uint32_t src_y,
                            uint32_t src_w, uint32_t src_h,
                            uint32_t dst_x, uint32_t dst_y,
                            uint32_t dst_w, uint32_t dst_h);

/**
 * @brief 禁用 Plane
 */
int ocr_drm_plane_disable(ocr_drm_planes_t *planes, ocr_drm_plane_t *plane);

/**
 * @brief 销毁 Plane 管理器
 */
void ocr_drm_planes_destroy(ocr_drm_planes_t *planes);

#endif /* OCR_DISPLAY_DRM_PLANE_H */
