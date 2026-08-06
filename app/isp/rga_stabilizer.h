#ifndef OCR_ISP_RGA_STABILIZER_H
#define OCR_ISP_RGA_STABILIZER_H
/**
 * @file rga_stabilizer.h
 * @brief 根据最终补偿参数执行固定窗口 RGA 裁剪与缩放
 *
 * 姿态轨迹平滑由 motion_compensate 完成。本模块不再重复低通，避免
 * 两级滤波造成补偿延迟。
 */

#include "buffer.h"
#include <stdint.h>

/** 防抖补偿参数（来自 motion_compensate） */
typedef struct {
    float dx;        /* 水平裁剪偏移（原始图像像素） */
    float dy;        /* 垂直裁剪偏移（原始图像像素） */
    float angle;     /* 保留字段；RGA 不执行任意角度旋转 */
    float scale;     /* 固定裁剪放大系数，例如 1.2 对应 1600x900 */
} ocr_stab_params_t;

/** 防抖器上下文 */
typedef struct {
    ocr_stab_params_t cur;    /* 当前实际使用的补偿参数 */
    ocr_stab_params_t target; /* motion_compensate 输出的最终参数 */
    float alpha;              /* 兼容字段；滤波已移到姿态层 */
    int   enabled;            /* 是否启用防抖 */
} ocr_rga_stabilizer_t;

/**
 * @brief 初始化防抖器
 * @param[in] stab   防抖器
 * @param[in] alpha  兼容参数，当前不在 RGA 层重复滤波
 */
int ocr_rga_stab_init(ocr_rga_stabilizer_t *stab, float alpha);

/**
 * @brief 更新最终补偿参数
 */
int ocr_rga_stab_update(ocr_rga_stabilizer_t *stab, const ocr_stab_params_t *params);

/**
 * @brief 从输入帧裁剪固定窗口并缩放到目标缓冲
 * @return 0=成功，负数=错误
 */
int ocr_rga_stab_apply(ocr_rga_stabilizer_t *stab, ocr_buffer_t *src, ocr_buffer_t *dst);

/**
 * @brief 启用/禁用防抖
 */
void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable);

#endif /* OCR_ISP_RGA_STABILIZER_H */
