#ifndef OCR_ISP_RGA_STABILIZER_H
#define OCR_ISP_RGA_STABILIZER_H
/**
 * @file rga_stabilizer.h
 * @brief 防抖补偿：根据 IMU 运动参数执行 RGA 变换
 *
 * 接收 motion_compensate 计算的平移/旋转量，
 * 通过 RGA 对图像执行反向变换以稳定画面。
 */

#include "buffer.h"
#include <stdint.h>

/** 防抖补偿参数（来自 motion_compensate） */
typedef struct {
    float dx;        /* 水平平移（像素） */
    float dy;        /* 垂直平移（像素） */
    float angle;     /* 旋转角度（度） */
    float scale;     /* 缩放系数（>1 放大以覆盖黑边） */
} ocr_stab_params_t;

/** 防抖器上下文 */
typedef struct {
    ocr_stab_params_t cur;    /* 当前补偿参数 */
    ocr_stab_params_t target; /* 目标参数（低通滤波后） */
    float alpha;              /* 滤波系数 */
    int   enabled;            /* 是否启用防抖 */
} ocr_rga_stabilizer_t;

/**
 * @brief 初始化防抖器
 * @param[in] stab   防抖器
 * @param[in] alpha  低通滤波系数 [0,1]
 */
int ocr_rga_stab_init(ocr_rga_stabilizer_t *stab, float alpha);

/**
 * @brief 更新目标补偿参数（来自 IMU 运动估计）
 */
int ocr_rga_stab_update(ocr_rga_stabilizer_t *stab, const ocr_stab_params_t *params);

/**
 * @brief 对输入帧执行防抖补偿（RGA 平移+旋转）
 * @param[in]     stab 防抖器
 * @param[in,out] buf  输入/输出缓冲（原地变换或输出到新缓冲）
 * @return 0=成功，负数=错误
 */
int ocr_rga_stab_apply(ocr_rga_stabilizer_t *stab, ocr_buffer_t *src, ocr_buffer_t *dst);

/**
 * @brief 启用/禁用防抖
 */
void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable);

#endif /* OCR_ISP_RGA_STABILIZER_H */
