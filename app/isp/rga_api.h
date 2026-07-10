#ifndef OCR_ISP_RGA_API_H
#define OCR_ISP_RGA_API_H
/**
 * @file rga_api.h
 * @brief RGA 硬件加速封装：wrapbuffer_fd/imresize/imcrop/imrotate/imtranslate
 *
 * 封装 Rockchip RGA（2D 图形加速器），通过 DMA-BUF fd 实现零拷贝图像变换。
 */

#include "buffer.h"

/**
 * @brief 通过 DMA-BUF fd 包装 RGA buffer 句柄
 * @param[in] fd       DMA-BUF fd
 * @param[in] width    像素宽
 * @param[in] height   像素高
 * @param[in] format   像素格式
 * @param[out] handle  RGA buffer 句柄（外部存储）
 * @return 0=成功，负数=错误
 */
int ocr_rga_wrap_fd(int fd, uint32_t width, uint32_t height,
                    ocr_pixel_format_t format, void *handle);

/**
 * @brief 缩放
 * @param[in] src      源 buffer
 * @param[in] dst      目标 buffer（尺寸已设定）
 * @return 0=成功，负数=错误
 */
int ocr_rga_resize(const ocr_buffer_t *src, ocr_buffer_t *dst);

/**
 * @brief 裁剪
 * @param[in] src   源 buffer
 * @param[in] dst   目标 buffer
 * @param[in] x,y,w,h 裁剪区域
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
