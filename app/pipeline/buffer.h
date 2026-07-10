#ifndef OCR_PIPELINE_BUFFER_H
#define OCR_PIPELINE_BUFFER_H
/**
 * @file buffer.h
 * @brief DMA-BUF 缓冲封装
 *
 * 统一管理 DMA-BUF fd、mmap 地址、引用计数、时间戳等，
 * 是零拷贝管线中数据流转的基本单元。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

/** 像素格式枚举 */
typedef enum {
    OCR_FMT_UNKNOWN = 0,
    OCR_FMT_NV12,       /* YUV 4:2:0 半平面 */
    OCR_FMT_NV16,       /* YUV 4:2:2 半平面 */
    OCR_FMT_YUYV,       /* YUV 4:2:2 打包 */
    OCR_FMT_RGB565,     /* RGB565 */
    OCR_FMT_RGB888,     /* RGB888 */
    OCR_FMT_ARGB8888,   /* ARGB8888 */
    OCR_FMT_BGRA8888,   /* BGRA8888 */
} ocr_pixel_format_t;

/** 缓冲区结构体 */
typedef struct {
    int               fd;        /* DMA-BUF 文件描述符（-1=无效） */
    void             *mmap_addr; /* mmap 映射地址（NULL=未映射） */
    size_t            size;      /* 缓冲区字节大小 */
    uint32_t          width;     /* 像素宽 */
    uint32_t          height;    /* 像素高 */
    ocr_pixel_format_t format;   /* 像素格式 */
    uint32_t          index;     /* 在缓冲池中的索引 */
    atomic_int        refcount;  /* 引用计数 */
    uint64_t          timestamp; /* 单调时间戳（纳秒） */
    uint64_t          frame_id;  /* 帧序号 */
    int               owned;     /* 1=由缓冲池分配（释放时归还），0=外部 */
} ocr_buffer_t;

/**
 * @brief 初始化缓冲区元数据（不分配内存）
 */
int ocr_buffer_init(ocr_buffer_t *buf);

/**
 * @brief 引用计数 +1
 * @return 新的引用计数值
 */
int ocr_buffer_ref(ocr_buffer_t *buf);

/**
 * @brief 引用计数 -1，归零时调用 release 回调
 * @return 剩余引用计数值（0=已释放）
 */
int ocr_buffer_unref(ocr_buffer_t *buf);

/**
 * @brief mmap 映射 DMA-BUF fd 到进程地址空间
 */
int ocr_buffer_mmap(ocr_buffer_t *buf);

/**
 * @brief 解除 mmap 映射
 */
void ocr_buffer_munmap(ocr_buffer_t *buf);

/**
 * @brief 像素格式转字符串（日志用）
 */
const char *ocr_pixel_format_str(ocr_pixel_format_t fmt);

#endif /* OCR_PIPELINE_BUFFER_H */
