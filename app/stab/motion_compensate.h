#ifndef OCR_STAB_MOTION_COMPENSATE_H
#define OCR_STAB_MOTION_COMPENSATE_H
/**
 * @file motion_compensate.h
 * @brief 平滑目标姿态与真实姿态之差转换为 RGA 裁剪偏移
 *
 * 维护一个缓慢跟随真实姿态的虚拟摄像机轨迹。快速姿态变化形成
 * 补偿角，缓慢的主动转动由虚拟轨迹逐渐跟随，避免裁剪窗口长期
 * 停留在边界。
 *
 * 平滑虚拟摄像机姿态与当前姿态之差，通过视场角换算成像素位移，限制在裁剪余量内。
 * scale 保持固定以避免画面呼吸；当前仅输出平移补偿，绕光轴的小角度旋转不进入 RGA 执行路径。
 */

#include "attitude_fusion.h"
#include "rga_stabilizer.h"

/** 运动补偿上下文 */
typedef struct {
    euler_t smooth_euler; /**< 低通跟随的虚拟摄像机姿态，三个角均为度；与实际姿态求差生成补偿。 */
    int     initialized; /**< 已用第一帧姿态建立平滑参考为 1；reset 置 0，下次首帧不立即补偿。 */
    float   hfov_deg; /**< 摄像头水平视场角，度；与图像宽度换算像素焦距。 */
    float   vfov_deg; /**< 摄像头垂直视场角，度；与图像高度换算垂直像素焦距。 */
    float   alpha; /**< 虚拟姿态跟随系数，0..1，无量纲；越小跟随越慢，瞬时反向补偿越大。 */
    float   max_shift_ratio; /**< 各轴最大补偿占图像尺寸的比例，0..0.5 以内；还受实际裁剪余量限制。 */
    float   max_angle_deg; /**< 预留的旋转补偿上限，度；当前仅校验范围，输出 angle 固定为 0。 */
    float   max_scale; /**< 固定裁剪放大系数，不逐帧呼吸变化；main 设为 1/stab_crop_ratio。 */
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
