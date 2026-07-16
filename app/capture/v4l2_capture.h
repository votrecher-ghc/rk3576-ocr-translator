#ifndef OCR_CAPTURE_V4L2_CAPTURE_H
#define OCR_CAPTURE_V4L2_CAPTURE_H
/**
 * @file v4l2_capture.h
 * @brief V4L2 采集封装：open/设置格式/REQBUFS/QBUF/DQBUF/流控
 *
 * 使用 MMAP 模式与 DMA-BUF 导出配合，实现零拷贝采集。
 */

#include <stdint.h>
#include <stdatomic.h>
#include "buffer.h"
#include "buffer_pool.h"
#include "thread.h"

#define V4L2_MAX_BUFS 8

/**
 * A callback receives one owned reference. Return 0 after transferring or
 * retaining that reference; return non-zero to let capture release it.
 */
typedef int (*ocr_v4l2_frame_cb_t)(ocr_buffer_t *buffer, void *user_data);

/** V4L2 采集上下文 */
typedef struct {
    int               fd;            /* V4L2 设备 fd */
    char              dev_name[64];  /* 设备节点路径 */
    uint32_t          width;         /* 采集宽度 */
    uint32_t          height;        /* 采集高度 */
    uint32_t          fps;           /* 帧率 */
    uint32_t          buffer_type;   /* enum v4l2_buf_type */
    uint32_t          bytes_per_line;
    uint32_t          size_image;
    ocr_pixel_format_t format;      /* 像素格式 */
    int               buf_count;     /* 缓冲区个数 */
    ocr_buffer_pool_t pool;          /* 缓冲池 */
    atomic_int        streaming;     /* 流式状态 */
    atomic_int        running;       /* 采集线程运行标志 */
    atomic_int        thread_started;/* 线程已创建，尚未 join */
    atomic_int        queue_error;   /* 队列状态不适合再次 STREAMON */
    ocr_thread_t      thread;        /* 采集线程 */
    uint64_t          frame_seq;     /* 帧序号 */
    ocr_mutex_t       io_lock;
    ocr_mutex_t       latest_lock;
    ocr_buffer_t     *latest_frame;
    ocr_v4l2_frame_cb_t frame_cb;
    void             *frame_user_data;
    int               initialized;
} ocr_v4l2_capture_t;

/**
 * @brief 打开 V4L2 设备并初始化
 * @param[in] cap      采集上下文
 * @param[in] dev_name 设备节点（如 /dev/video0）
 * @param[in] width    期望宽度
 * @param[in] height   期望高度
 * @param[in] fps      期望帧率
 * @param[in] format   像素格式
 * @return 0=成功，负数=错误
 */
int ocr_v4l2_open(ocr_v4l2_capture_t *cap, const char *dev_name,
                  uint32_t width, uint32_t height, uint32_t fps,
                  ocr_pixel_format_t format);

/** Same as ocr_v4l2_open with an explicit requested buffer count. */
int ocr_v4l2_open_ex(ocr_v4l2_capture_t *cap, const char *dev_name,
                     uint32_t width, uint32_t height, uint32_t fps,
                     ocr_pixel_format_t format, int buffer_count);

/** Set the downstream callback while capture is stopped. */
int ocr_v4l2_set_frame_callback(ocr_v4l2_capture_t *cap,
                                ocr_v4l2_frame_cb_t callback,
                                void *user_data);

/** Return a new reference to the newest frame, or NULL if none was captured. */
ocr_buffer_t *ocr_v4l2_get_latest(ocr_v4l2_capture_t *cap);

/**
 * @brief 启动采集（streamon + 采集线程）
 * @return 0=成功，负数=错误
 */
int ocr_v4l2_start(ocr_v4l2_capture_t *cap);

/**
 * @brief 停止采集
 */
int ocr_v4l2_stop(ocr_v4l2_capture_t *cap);

/**
 * @brief 关闭 V4L2 设备并释放资源
 */
int ocr_v4l2_close(ocr_v4l2_capture_t *cap);

/**
 * @brief 设置帧率
 */
int ocr_v4l2_set_fps(ocr_v4l2_capture_t *cap, uint32_t fps);

#endif /* OCR_CAPTURE_V4L2_CAPTURE_H */
