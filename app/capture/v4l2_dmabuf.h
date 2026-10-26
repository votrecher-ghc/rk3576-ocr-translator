#ifndef OCR_CAPTURE_V4L2_DMABUF_H
#define OCR_CAPTURE_V4L2_DMABUF_H
/**
 * @file v4l2_dmabuf.h
 * @brief V4L2 DMA-BUF 导出（VIDIOC_EXPBUF）
 *
 * 为已有 V4L2 MMAP 缓冲导出共享 fd；导出操作不分配另一幅图像，也不等待采集完成。
 * 使用方仍须遵守 DQBUF/QBUF 时序；成功返回的 fd 需要由调用方或接管它的缓冲池关闭。
 */

#include <stdint.h>

/**
 * @brief 从 V4L2 缓冲区导出 DMA-BUF fd
 * @param[in] v4l2_fd  V4L2 设备 fd
 * @param[in] index    缓冲区索引
 * @return DMA-BUF fd，负数=错误
 */
int ocr_v4l2_export_dmabuf(int v4l2_fd, int index);

/** Export a specific plane from a single- or multi-planar queue. */
int ocr_v4l2_export_dmabuf_ex(int v4l2_fd, uint32_t buffer_type,
                              int index, uint32_t plane);

#endif /* OCR_CAPTURE_V4L2_DMABUF_H */
