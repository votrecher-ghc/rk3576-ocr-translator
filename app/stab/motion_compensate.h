#ifndef OCR_STAB_MOTION_COMPENSATE_H
#define OCR_STAB_MOTION_COMPENSATE_H
/**
 * @file motion_compensate.h
 * @brief 平滑目标姿态与真实姿态之差转换为 RGA 裁剪偏移
 *
 * 维护一个缓慢跟随真实姿态的虚拟摄像机轨迹。快速姿态变化形成
 * 补偿角，缓慢的主动转动由虚拟轨迹逐渐跟随，避免裁剪窗口长期
 * 停留在边界。
 */

#include "attitude_fusion.h"
#include "rga_stabilizer.h"

/** 运动补偿上下文 */
typedef struct {
    euler_t smooth_euler;      /* 平滑目标姿态 */
    int     initialized;       /* 平滑姿态是否已初始化 */
    float   hfov_deg;          /* 水平视场角（度） */
    float   vfov_deg;          /* 垂直视场角（度） */
    float   alpha;             /* 目标姿态跟随系数；越小防抖越强 */
    float   max_shift_ratio;   /* 单轴最大平移占图像尺寸的比例 */
    float   max_angle_deg;     /* 保留字段；RGA 不执行任意角度旋转 */
    float   max_scale;         /* 固定裁剪放大系数 */
} ocr_motion_comp_t;

/**
 * @brief 初始化运动补偿
 * @param[in] mc      运动补偿上下文
 * @param[in] hfov    水平视场角（度）
 * @param[in] vfov    垂直视场角（度）
 * @param[in] alpha   平滑目标姿态跟随系数 [0,1]
 */
int ocr_motion_comp_init(ocr_motion_comp_t *mc, float hfov, float vfov, float alpha);

/**
 * @brief 根据当前姿态更新补偿参数
 * @param[in] mc    运动补偿上下文
 * @param[in] att   当前姿态
 * @param[in] img_w 原始图像宽（像素）
 * @param[in] img_h 原始图像高（像素）
 * @param[out] params 输出补偿参数
 * @return 0=成功，负数=错误
 */
int ocr_motion_comp_update(ocr_motion_comp_t *mc, const ocr_attitude_t *att,
                           uint32_t img_w, uint32_t img_h,
                           ocr_stab_params_t *params);

/**
 * @brief 重置运动补偿并在下一帧重新建立目标姿态
 */
void ocr_motion_comp_reset(ocr_motion_comp_t *mc);

#endif /* OCR_STAB_MOTION_COMPENSATE_H */
