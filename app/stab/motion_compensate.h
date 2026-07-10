#ifndef OCR_STAB_MOTION_COMPENSATE_H
#define OCR_STAB_MOTION_COMPENSATE_H
/**
 * @file motion_compensate.h
 * @brief 运动估计：帧间姿态差→像素平移+旋转角→低通滤波
 *
 * 根据相邻帧的姿态差计算图像平移和旋转量，输出给 RGA 防抖器。
 */

#include "attitude_fusion.h"
#include "rga_stabilizer.h"

/** 运动补偿上下文 */
typedef struct {
    euler_t prev_euler;    /* 上一帧欧拉角 */
    int     has_prev;      /* 是否有上一帧 */
    float   hfov_deg;      /* 水平视场角（度） */
    float   vfov_deg;      /* 垂直视场角（度） */
    float   alpha;         /* 低通滤波系数 */
    ocr_stab_params_t filtered; /* 滤波后补偿参数 */
} ocr_motion_comp_t;

/**
 * @brief 初始化运动补偿
 * @param[in] mc      运动补偿上下文
 * @param[in] hfov    水平视场角（度）
 * @param[in] vfov    垂直视场角（度）
 * @param[in] alpha   低通滤波系数
 */
int ocr_motion_comp_init(ocr_motion_comp_t *mc, float hfov, float vfov, float alpha);

/**
 * @brief 根据当前姿态更新补偿参数
 * @param[in] mc    运动补偿上下文
 * @param[in] att   当前姿态
 * @param[in] img_w 图像宽（像素）
 * @param[in] img_h 图像高（像素）
 * @param[out] params 输出补偿参数
 * @return 0=成功，负数=错误
 */
int ocr_motion_comp_update(ocr_motion_comp_t *mc, const ocr_attitude_t *att,
                           uint32_t img_w, uint32_t img_h,
                           ocr_stab_params_t *params);

/**
 * @brief 重置运动补偿（重置参考帧）
 */
void ocr_motion_comp_reset(ocr_motion_comp_t *mc);

#endif /* OCR_STAB_MOTION_COMPENSATE_H */
