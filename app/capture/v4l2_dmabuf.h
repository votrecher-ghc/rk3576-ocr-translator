#ifndef OCR_CAPTURE_V4L2_DMABUF_H
#define OCR_CAPTURE_V4L2_DMABUF_H
/**
 * @file v4l2_dmabuf.h
 * @brief V4L2 DMA-BUF 导出（VIDIOC_EXPBUF）
 */

/**
 * @brief 从 V4L2 缓冲区导出 DMA-BUF fd
 * @param[in] v4l2_fd  V4L2 设备 fd
 * @param[in] index    缓冲区索引
 * @return DMA-BUF fd，负数=错误
 */
int ocr_v4l2_export_dmabuf(int v4l2_fd, int index);

#endif /* OCR_CAPTURE_V4L2_DMABUF_H */
