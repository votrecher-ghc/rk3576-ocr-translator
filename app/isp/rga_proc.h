#ifndef OCR_ISP_RGA_PROC_H
#define OCR_ISP_RGA_PROC_H
/**
 * @file rga_proc.h
 * @brief 图像处理节点：格式转换/裁剪/缩放
 *
 * 作为管线节点，对输入帧执行 RGA 变换后输出到下游。
 *
 * RGA 节点从输出池取得空闲缓冲，继承帧号/时间戳，完成缩放或转换后发布新图像。
 * 输入由节点借给回调；输出由 emit 转移所有权，返回正数防止节点再次转发原始输入。
 */

#include "node.h"
#include "buffer_pool.h"
#include "v4l2_capture.h"

/** RGA 处理上下文 */
typedef struct {
    ocr_buffer_pool_t *out_pool;   /**< 借用的独立输出池，提供 RGA 可写缓冲；处理器不负责销毁池。 */
    uint32_t           out_width;  /**< 目标有效宽度，像素；须与 out_pool 每块内存的实际布局一致。 */
    uint32_t           out_height; /**< 目标有效高度，像素；修改它不会自动重新分配输出内存。 */
    ocr_pixel_format_t out_format; /**< 目标像素格式，用于校验输出池及选择等尺寸转色路径。 */
    int                do_cvtcolor;/**< 为 1 且等尺寸、格式不同时走 cvtcolor；否则走 resize 组合处理。 */
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
