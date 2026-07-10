#ifndef OCR_ISP_RGA_PROC_H
#define OCR_ISP_RGA_PROC_H
/**
 * @file rga_proc.h
 * @brief 图像处理节点：格式转换/裁剪/缩放
 *
 * 作为管线节点，对输入帧执行 RGA 变换后输出到下游。
 */

#include "node.h"
#include "buffer_pool.h"
#include "v4l2_capture.h"

/** RGA 处理上下文 */
typedef struct {
    ocr_buffer_pool_t *out_pool;   /* 输出缓冲池 */
    uint32_t           out_width;  /* 输出宽 */
    uint32_t           out_height; /* 输出高 */
    ocr_pixel_format_t out_format; /* 输出格式 */
    int                do_cvtcolor;/* 是否执行格式转换 */
} ocr_rga_proc_t;

/**
 * @brief 初始化 RGA 处理上下文
 */
int ocr_rga_proc_init(ocr_rga_proc_t *proc, ocr_buffer_pool_t *pool,
                      uint32_t out_w, uint32_t out_h, ocr_pixel_format_t out_fmt);

/**
 * @brief RGA 处理节点回调（注册到 ocr_pipeline_node_t.process）
 *        执行格式转换 + 缩放到输出尺寸
 */
int ocr_rga_proc_process(ocr_pipeline_node_t *node, ocr_buffer_t *buf);

#endif /* OCR_ISP_RGA_PROC_H */
