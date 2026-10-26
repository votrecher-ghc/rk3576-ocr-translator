#ifndef OCR_DISPLAY_DRM_SINK_H
#define OCR_DISPLAY_DRM_SINK_H

/** @file drm_sink.h @brief Pipeline sink that presents DMA-BUF frames.
 *
 * 显示节点额外持有当前扫描帧的引用，提交下一帧并确认切换后才释放上一帧。
 * framebuffer 缓存按缓冲对象及布局复用；销毁前先停止扫描，避免输出池过早复用图像。
 */

#include "drm_fb.h"
#include "drm_plane.h"
#include "node.h"

#define OCR_DRM_SINK_MAX_FBS 32

typedef struct {
    const ocr_buffer_t *source; /**< 借用的稳定池元数据地址，作为 FB 缓存身份；不因缓存条目存在而增加应用引用。 */
    int source_fd; /**< 创建缓存时的 DMA-BUF fd 数值快照；联合对象地址/布局匹配，不能只按 fd 命中。 */
    size_t source_size; /**< 缓存建立时的分配字节数快照，避免布局变化后误用旧 framebuffer。 */
    uint32_t width, /**< 缓存图像有效宽度快照，像素；用于核对本次缓冲布局是否仍匹配。 */
             height; /**< 缓存图像有效高度快照，像素；与 width 一起校验 FB 可复用性。 */
    ocr_pixel_format_t format; /**< 缓存图像像素格式快照，决定 FourCC 及各平面解释。 */
    uint32_t plane_count; /**< 缓存逻辑平面数快照，区别于物理内存块数量。 */
    uint32_t strides[4]; /**< 各平面字节行跨度快照，供布局匹配使用。 */
    uint32_t offsets[4]; /**< 各平面起点字节偏移快照，与 strides 共同构成缓存匹配条件。 */
    ocr_drm_fb_t fb; /**< 本条目拥有的 PRIME 导入/GEM/framebuffer 状态，淘汰或 sink 销毁时释放。 */
    int valid; /**< FB 及缓存元数据建立成功标志；为 0 的条目可供新缓冲使用。 */
} ocr_drm_sink_entry_t;

typedef struct {
    ocr_drm_device_t *dev; /**< 借用的 DRM 设备，sink 销毁并停止扫描之前必须保持打开。 */
    ocr_drm_planes_t *planes; /**< 借用的层管理器，用其 primary 承载视频；不在 sink 中销毁管理器。 */
    ocr_drm_sink_entry_t entries[OCR_DRM_SINK_MAX_FBS]; /**< 本 sink 拥有的固定容量 FB 缓存；源池对象须在缓存销毁前保持有效。 */
    ocr_buffer_t *current; /**< 当前扫描图像，额外持有一份应用引用；切到下一帧或解除扫描后才 unref。 */
    ocr_drm_sink_entry_t *current_entry; /**< 指向 entries 中正在扫描的 FB 条目，缓存淘汰必须跳过它。 */
    uint64_t current_frame_id; /**< 成功提交 current 时的帧号快照，联合指针判断是否重复呈现同一帧。 */
    unsigned int next_evict; /**< 下一次淘汰搜索的起始条目下标，轮转更新，不表示视频帧号。 */
    int initialized; /**< sink 可用标志；停止扫描或清理失败时保留它，以便继续持有资源并重试。 */
} ocr_drm_sink_t;

int ocr_drm_sink_init(ocr_drm_sink_t *sink, ocr_drm_device_t *dev,
                      ocr_drm_planes_t *planes);
/** @brief 额外引用新帧，确认切换成功后替换 current 并释放旧帧；失败则撤销新引用。 */
int ocr_drm_sink_present(ocr_drm_sink_t *sink, ocr_buffer_t *buffer);
int ocr_drm_sink_process(ocr_pipeline_node_t *node, ocr_buffer_t *buffer);
/** @brief 先解除 primary 扫描输出，再释放当前帧和 framebuffer 缓存。 */
void ocr_drm_sink_destroy(ocr_drm_sink_t *sink);

#endif /* OCR_DISPLAY_DRM_SINK_H */
