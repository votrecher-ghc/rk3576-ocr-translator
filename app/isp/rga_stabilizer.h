#ifndef OCR_ISP_RGA_STABILIZER_H
#define OCR_ISP_RGA_STABILIZER_H
/**
 * @file rga_stabilizer.h
 * @brief 根据最终补偿参数执行固定窗口 RGA 裁剪与缩放
 *
 * 姿态轨迹平滑由 motion_compensate 完成。本模块不再重复低通，避免
 * 两级滤波造成补偿延迟。
 *
 * 电子防抖执行器：把目标位移转换为固定尺寸裁剪窗口，再由同步 RGA 裁剪/缩放到输出。
 * 裁剪位置按色度采样对齐并限制在源图范围内；当前不执行任意小角度图像旋转。
 */

#include "buffer.h"
#include <stdint.h>

/** 防抖补偿参数（来自 motion_compensate） */
typedef struct {
    float dx;        /**< 水平补偿量，源图像素；裁剪左上角为居中位置减 dx，正值向左移动窗口。 */
    float dy;        /**< 垂直补偿量，源图像素；裁剪左上角为居中位置减 dy，正值向上移动窗口。 */
    float angle;     /**< 旋转补偿角，度；当前执行器只记录/提示，未执行任意小角度旋转。 */
    float scale;     /**< 无量纲固定放大倍数，执行时限制到 1..2；1920x1080/1.2 得 1600x900。 */
} ocr_stab_params_t;

/** 防抖器上下文 */
typedef struct {
    ocr_stab_params_t cur;    /**< 最近执行帧所用参数，由 apply 直接复制 target；不再二次低通。 */
    ocr_stab_params_t target; /**< 姿态层生成的待执行参数，update 写入，下一次 apply 读取。 */
    float alpha;              /**< 保留的兼容低通系数，初始化限制到 0..1；当前 apply 不使用它滤波。 */
    int   enabled;            /**< 非零执行补偿裁剪，为 0 时仅将源图缩放到目标尺寸。 */
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
