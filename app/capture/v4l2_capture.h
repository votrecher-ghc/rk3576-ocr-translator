#ifndef OCR_CAPTURE_V4L2_CAPTURE_H
#define OCR_CAPTURE_V4L2_CAPTURE_H
/**
 * @file v4l2_capture.h
 * @brief V4L2 采集封装：open/设置格式/REQBUFS/QBUF/DQBUF/流控
 *
 * 使用 MMAP 模式与 DMA-BUF 导出配合，实现零拷贝采集。
 *
 * 接口所有权：帧回调成功时接管一个引用，get_latest 返回一个新引用，使用完必须 unref。
 * 采集线程只在 DQBUF 后发布有效帧；缓冲最后一个引用释放后，回收回调才会重新 QBUF。
 */

#include <stdint.h>
#include <stdatomic.h>
#include "buffer.h"
#include "buffer_pool.h"
#include "thread.h"

#define V4L2_MAX_BUFS 8

/**
 * @brief 帧回调收到一个归其管理的引用。
 * @return 0 表示已保留或转交该引用；非零表示由采集模块释放这份引用。
 * @note 在采集线程中同步调用，通常只向 RGA 队列传指针，避免阻塞后续 DQBUF。
 *       入队成功后不再 unref 这份引用；入队失败直接返回非零，由采集模块收尾。
 */
typedef int (*ocr_v4l2_frame_cb_t)(ocr_buffer_t *buffer, void *user_data);

/** V4L2 采集上下文 */
typedef struct {
    int               fd;            /* 本采集器拥有的 /dev/videoX fd，关闭采集器时释放；区别于每帧 DMA-BUF fd。 */
    char              dev_name[64];  /* 设备节点路径的值副本，含结尾 NUL；用于打开设备和日志。 */
    uint32_t          width;         /**< S_FMT 协商后的有效宽度，像素；不一定等于请求宽度。 */
    uint32_t          height;        /**< S_FMT 协商后的有效高度，像素；用于 Y/UV 布局和下游处理。 */
    uint32_t          fps;           /**< S_PARM 协商后帧率的整数值，帧/秒；失败时保留请求值。 */
    uint32_t          buffer_type;   /**< VIDEO_CAPTURE 或 CAPTURE_MPLANE，所有队列 ioctl 必须一致。 */
    uint32_t          bytes_per_line; /* 驱动协商后的字节行跨度，不能假定等于宽度。 */
    uint32_t          size_image;    /* 驱动协商的 sizeimage，字节；实际槽位映射大小还须看 QUERYBUF。 */
    ocr_pixel_format_t format;      /* 本次采集的像素格式；驱动若协商成不匹配格式则初始化失败。 */
    int               buf_count;     /**< REQBUFS 实际分配数，2..8；驱动可以调整请求数量。 */
    ocr_buffer_pool_t pool;          /**< 拥有各槽导出的 fd 及 CPU 映射，最后一次引用回收时通知 QBUF。 */
    atomic_int        streaming;     /**< 内核采集流启用标志；回收线程据此决定是否重新 QBUF。 */
    atomic_int        running;       /**< 采集线程继续循环的请求，停止或致命错误置 0，不表示已完成 join。 */
    atomic_int        thread_started;/* 线程已创建，尚未 join */
    atomic_int        queue_error;   /* QBUF/STREAMON/STREAMOFF 等队列操作失败的标记；为 1 时拒绝再次启动。 */
    ocr_thread_t      thread;        /* 执行等待帧、DQBUF 和发布帧的工作线程；thread_started 时需要 join。 */
    uint64_t          frame_seq;     /**< 采集线程生成下一有效帧的序号，从 0 递增；不是槽位索引。 */
    ocr_mutex_t       io_lock;      /* 串行化 QBUF/DQBUF 与流启停。 */
    ocr_mutex_t       latest_lock;  /* 保护最新帧指针的替换和加引用操作。 */
    ocr_buffer_t     *latest_frame; /* 额外持有最新帧，替换或停止时释放旧引用。 */
    ocr_v4l2_frame_cb_t frame_cb;   /* 通常向 RGA 节点入队，不复制像素。 */
    void             *frame_user_data; /**< frame_cb 的借用上下文，主流程指向 RGA 节点；采集中不更换。 */
    int               initialized; /**< 设备、池与锁全部建立完成；尚不代表 STREAMON 或线程已启动。 */
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

/**
 * @brief 初始化采集设备并建立 MMAP / DMA-BUF 共享缓冲，尚不启动采集。
 * @param[out] cap 尚未初始化的上下文；成功后读取其 width/height/stride 获取协商结果。
 * @param[in] dev_name V4L2 采集节点，例如 /dev/video0，不是 /dev/dma_heap/system。
 * @param[in] width,height,fps 请求图像宽高和帧率；实际尺寸/帧率由驱动协商。
 * @param[in] format 请求像素格式；若驱动静默换成别的格式，本实现拒绝初始化。
 * @param[in] buffer_count 请求缓冲数，范围 2..V4L2_MAX_BUFS，实际数以 cap->buf_count 为准。
 * @return 0 成功；-1 参数不合法；-2 设备、格式、分配或映射等初始化步骤失败。
 * @note 流程为 REQBUFS -> QUERYBUF -> mmap(video fd) -> EXPBUF -> pool_register。
 *       每个槽位只导出一个 fd；不支持 Y/UV 分属两个 V4L2 内存平面的布局。
 *       成功后先设置 frame callback，再 start；失败时内部回收本次已获取资源。
 */
int ocr_v4l2_open_ex(ocr_v4l2_capture_t *cap, const char *dev_name,
                     uint32_t width, uint32_t height, uint32_t fps,
                     ocr_pixel_format_t format, int buffer_count);

/** @brief 在线程尚未创建或已完成 join 时设置下游回调，不允许运行中无锁替换。 */
int ocr_v4l2_set_frame_callback(ocr_v4l2_capture_t *cap,
                                ocr_v4l2_frame_cb_t callback,
                                void *user_data);

/**
 * @brief 在最新帧指针锁内取帧并增加一份引用，不复制像素。
 * @return 有帧时返回调用者拥有的引用，无帧或上下文无效时返回 NULL。
 * @note 用完必须 ocr_buffer_unref；此引用会推迟 QBUF，长期持有会减少可采集缓冲。
 */
ocr_buffer_t *ocr_v4l2_get_latest(ocr_v4l2_capture_t *cap);

/**
 * @brief 启动采集（streamon + 采集线程）
 * @return 0=成功，负数=错误
 */
int ocr_v4l2_start(ocr_v4l2_capture_t *cap);

/**
 * @brief 停止采集
 * @return 0 已停止；-2 队列/关流失败；-3 无法回收线程（例如线程试图 join 自身）。
 * @note 停流、join 并释放 latest_frame 引用，不强行撤销下游持有的图像引用。
 */
int ocr_v4l2_stop(ocr_v4l2_capture_t *cap);

/**
 * @brief 关闭 V4L2 设备并释放资源
 * @return 0 完成；-2 下游仍持有采集帧，关闭延期；-3 线程尚未安全回收。
 * @note 下游释放完后可重试。不要在失败后销毁 cap 存储，回收回调仍会引用它。
 */
int ocr_v4l2_close(ocr_v4l2_capture_t *cap);

/**
 * @brief 设置帧率
 */
int ocr_v4l2_set_fps(ocr_v4l2_capture_t *cap, uint32_t fps);

#endif /* OCR_CAPTURE_V4L2_CAPTURE_H */
