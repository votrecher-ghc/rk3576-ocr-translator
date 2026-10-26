#ifndef OCR_DISPLAY_DRM_FB_H
#define OCR_DISPLAY_DRM_FB_H
/**
 * @file drm_fb.h
 * @brief DRM framebuffer：DMA-BUF 绑定（drmPrimeFDToHandle + drmModeAddFB2）
 *
 * 把 DMA-BUF 导入为 GEM handle，并根据像素格式、stride 和 offset 创建 framebuffer。
 * 导入不复制像素，源 fd 只是借用；翻页接口等待新帧切换完成，供调用方安全释放上一帧。
 */

#include "buffer.h"
#include "drm_device.h"

/** DRM framebuffer 句柄 */
typedef struct {
    uint32_t fb_id; /**< AddFB2 创建的 framebuffer 对象 ID，RmFB 归还；0 表示尚未建立。 */
    uint32_t handle; /**< 此 DRM fd 上 PRIME 导入的 GEM handle；本模块关闭，区别于可跨进程传的 fd。 */
    uint32_t handles[4]; /**< 各逻辑平面使用的 GEM handle；单 DMA-BUF 的 NV12 Y/UV 复用同一个值。 */
    uint32_t pitches[4]; /**< 各平面一行占用字节数，含 padding；传给 AddFB2，不是 RGA 像素 wstride。 */
    uint32_t offsets[4]; /**< 各平面相对共享存储起点的字节偏移，NV12 的 UV 通常从 Y 分配区之后开始。 */
    uint32_t plane_count; /**< 像素格式所需的有效逻辑平面数，不表示有几块独立 DMA-BUF。 */
    uint32_t width; /**< 有效图像宽度，像素；不包含行尾填充。 */
    uint32_t height; /**< 有效图像高度，像素；用于校验与当前显示模式匹配。 */
    ocr_pixel_format_t format; /**< 项目像素格式，用于转换 DRM FourCC 和推导平面布局。 */
    int      dmabuf_fd; /**< 借用输出池的 DMA-BUF fd，FB 销毁不 close；-1 表示未绑定。 */
} ocr_drm_fb_t;

/**
 * @brief 从 DMA-BUF fd 创建 DRM framebuffer
 * @param[in] dev   DRM 设备
 * @param[in] buf   缓冲区（含 fd/width/height/format）
 * @param[out] fb   framebuffer 句柄
 * @return 0=成功，负数=错误
 */
int ocr_drm_fb_create(ocr_drm_device_t *dev, const ocr_buffer_t *buf, ocr_drm_fb_t *fb);

/**
 * @brief 使用显式 stride/offset 从 DMA-BUF 创建 framebuffer
 *
 * pitches/offsets 可传 NULL 使用紧密排列默认值。NV12/NV16 的两个格式平面
 * 共享同一个 DMA-BUF/GEM handle。
 */
int ocr_drm_fb_create_with_layout(ocr_drm_device_t *dev,
                                  const ocr_buffer_t *buf,
                                  const uint32_t pitches[4],
                                  const uint32_t offsets[4],
                                  ocr_drm_fb_t *fb);

/**
 * @brief 销毁 framebuffer（drmModeRmFB + gem close）
 */
int ocr_drm_fb_destroy(ocr_drm_device_t *dev, ocr_drm_fb_t *fb);

/**
 * @brief pageflip：同步切换到指定 framebuffer
 * @param[in] dev     DRM 设备
 * @param[in] fb      目标 framebuffer
 * @param[in] vblank_cb vblank 回调（可为 NULL）
 * @return 0=成功，负数=错误
 *
 * 首帧会同步执行 modeset；后续帧使用 DRM_MODE_PAGE_FLIP_EVENT，并在
 * 收到该次 flip 的完成事件后返回。返回后可安全回收上一帧。
 */
int ocr_drm_fb_pageflip(ocr_drm_device_t *dev, ocr_drm_fb_t *fb,
                        void (*vblank_cb)(int, uint32_t, uint32_t, void *),
                        void *user_data);

#endif /* OCR_DISPLAY_DRM_FB_H */
