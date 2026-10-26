#ifndef OCR_ISP_RGA_API_H
#define OCR_ISP_RGA_API_H
/**
 * @file rga_api.h
 * @brief RGA 硬件加速封装：wrapbuffer_fd/imresize/imcrop/imrotate/imtranslate
 *
 * 封装 Rockchip RGA（2D 图形加速器），通过 DMA-BUF fd 实现零拷贝图像变换。
 *
 * 输入、输出通过 DMA-BUF fd 引用内存，布局由宽高、格式和行跨度描述。
 * 所有图像操作采用同步模式；成功返回代表本次 RGA 读写完成，调用方仍负责引用和缓冲复用。
 */

#include "buffer.h"

/**
 * @brief 通过 DMA-BUF fd 包装 RGA buffer 句柄
 * @param[in] fd       DMA-BUF fd
 * @param[in] width    像素宽
 * @param[in] height   像素高
 * @param[in] format   像素格式
 * @param[out] handle  指向调用者提供的 rga_buffer_t 存储，并非新创建的硬件句柄。
 * @return 0=成功，负数=错误
 * @note 仅包装描述，不处理图像、不 dup fd；简化接口假定无额外行/高度填充。
 */
int ocr_rga_wrap_fd(int fd, uint32_t width, uint32_t height,
                    ocr_pixel_format_t format, void *handle);

/**
 * @brief 通过共享 fd 同步缩放输入图像，支持的格式组合由 librga 决定。
 * @param[in] src 已完成生产的源图，fd 及实际 stride/offset 需有效。
 * @param[in,out] dst 已分配且可独占写入的目标图，其有效宽高决定缩放结果。
 * @return 0 成功；-1 参数非法；-2/-3 源/目标布局无法包装；-4 RGA 操作失败。
 * @note 内部 sync=1，成功返回后才可读取 dst 或释放本次对 src 的使用引用。
 *       不申请输出内存、不修改引用计数、不发送 CPU 缓存同步，调用者负责资源交接。
 */
int ocr_rga_resize(const ocr_buffer_t *src, ocr_buffer_t *dst);

/**
 * @brief 裁剪
 * @param[in] src   源 buffer
 * @param[in] dst   目标 buffer
 * @param[in] x,y,w,h 源图内的像素坐标和范围，不能传字节偏移或目标图坐标。
 * @note NV12 的四个参数须为偶数；NV16/YUYV 的 x/w 须为偶数；执行采用同步模式。
 */
int ocr_rga_crop(const ocr_buffer_t *src, ocr_buffer_t *dst,
                 uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/** 旋转角度枚举 */
typedef enum {
    OCR_RGA_ROTATE_0   = 0,
    OCR_RGA_ROTATE_90  = 90,
    OCR_RGA_ROTATE_180 = 180,
    OCR_RGA_ROTATE_270 = 270,
} ocr_rga_rotate_t;

/**
 * @brief 旋转
 */
int ocr_rga_rotate(const ocr_buffer_t *src, ocr_buffer_t *dst, ocr_rga_rotate_t angle);

/**
 * @brief 平移
 */
int ocr_rga_translate(const ocr_buffer_t *src, ocr_buffer_t *dst,
                      int32_t dx, int32_t dy);

/**
 * @brief 格式转换（如 NV12→ARGB8888）
 */
int ocr_rga_cvtcolor(const ocr_buffer_t *src, ocr_buffer_t *dst);

#endif /* OCR_ISP_RGA_API_H */
