#ifndef OCR_PIPELINE_ZEROCOPY_H
#define OCR_PIPELINE_ZEROCOPY_H
/**
 * @file zerocopy.h
 * @brief DMA-BUF fd 跨模块零拷贝传递封装
 *
 * DMA-BUF fd 在同一进程内可直接借用；需要独立关闭时可 dup 后使用，
 * 跨进程需通过 SCM_RIGHTS 发送。本模块提供统一接口。
 *
 * dup 或 SCM_RIGHTS 传递的是共享内存的引用，不复制像素，也不创建另一块图像内存。
 * DMA_BUF_IOCTL_SYNC 标记 CPU 访问区间并维护缓存一致性，不替代硬件完成等待和访问互斥。
 */

#include "buffer.h"

/**
 * @brief 复制 DMA-BUF fd（同进程内共享）
 * @param[in] fd 原 fd
 * @return 新 fd，负数=错误
 */
int ocr_zerocopy_dup_fd(int fd);

/**
 * @brief 通过 SCM_RIGHTS 跨进程发送 fd（unix socket）
 * @param[in] sock_fd unix socket fd
 * @param[in] fd      待发送的 DMA-BUF fd
 * @return 0=成功，负数=错误
 */
int ocr_zerocopy_send_fd(int sock_fd, int fd);

/**
 * @brief 通过 SCM_RIGHTS 跨进程接收 fd
 * @param[in] sock_fd unix socket fd
 * @return 接收到的 fd，负数=错误
 */
int ocr_zerocopy_recv_fd(int sock_fd);

/**
 * @brief 标记 CPU 访问 DMA-BUF 的开始/结束，维护 CPU 与设备间的缓存一致性
 * @param[in] fd    DMA-BUF fd
 * @param[in] sync  0=开始读，1=结束读，2=开始写，3=结束写
 * @return 0=成功，负数=错误
 * @note 读写方向均相对于 CPU。先等待生产者完成，再 START；CPU 访问后配对 END。
 *       此接口不提供访问互斥，也不能替代 RGA 完成等待或显示翻页完成通知。
 */
int ocr_zerocopy_sync(int fd, int sync);

#endif /* OCR_PIPELINE_ZEROCOPY_H */
