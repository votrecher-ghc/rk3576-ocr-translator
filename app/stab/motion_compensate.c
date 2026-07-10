/**
 * @file motion_compensate.c
 * @brief 运动估计实现
 */
#include "motion_compensate.h"
#include "math_utils.h"
#include "log.h"

#include <string.h>
#include <math.h>

int ocr_motion_comp_init(ocr_motion_comp_t *mc, float hfov, float vfov, float alpha)
{
    if (!mc) return -1;
    memset(mc, 0, sizeof(*mc));
    mc->hfov_deg = hfov;
    mc->vfov_deg = vfov;
    mc->alpha = ocr_clamp_f(alpha, 0.0f, 1.0f);
    mc->has_prev = 0;
    return 0;
}

int ocr_motion_comp_update(ocr_motion_comp_t *mc, const ocr_attitude_t *att,
                           uint32_t img_w, uint32_t img_h,
                           ocr_stab_params_t *params)
{
    if (!mc || !att || !params) return -1;

    euler_t cur;
    ocr_attitude_to_euler(att, &cur);

    if (!mc->has_prev) {
        /* 第一帧：建立参考 */
        mc->prev_euler = cur;
        mc->has_prev = 1;
        memset(params, 0, sizeof(*params));
        params->scale = 1.0f;
        return 0;
    }

    /* 帧间姿态差（度） */
    float d_pitch = cur.pitch - mc->prev_euler.pitch;
    float d_roll  = cur.roll  - mc->prev_euler.roll;
    float d_yaw   = cur.yaw   - mc->prev_euler.yaw;

    /* 姿态差 → 像素平移
     * pitch 变化 → 垂直方向像素移动
     * yaw 变化   → 水平方向像素移动 */
    float dx = -d_yaw * (float)img_w / mc->hfov_deg;
    float dy = -d_pitch * (float)img_h / mc->vfov_deg;
    float angle = -d_roll; /* roll → 图像旋转 */

    /* 低通滤波平滑 */
    mc->filtered.dx    = ocr_lowpass(mc->filtered.dx,    dx,    mc->alpha);
    mc->filtered.dy    = ocr_lowpass(mc->filtered.dy,    dy,    mc->alpha);
    mc->filtered.angle = ocr_lowpass(mc->filtered.angle, angle, mc->alpha);

    /* 缩放系数：根据平移量自适应放大以覆盖黑边 */
    float max_shift = fabsf(mc->filtered.dx) > fabsf(mc->filtered.dy) ?
                      fabsf(mc->filtered.dx) : fabsf(mc->filtered.dy);
    mc->filtered.scale = 1.0f + max_shift / (float)img_w;
    mc->filtered.scale = ocr_clamp_f(mc->filtered.scale, 1.0f, 1.3f);

    *params = mc->filtered;

    /* 更新参考帧 */
    mc->prev_euler = cur;
    return 0;
}

void ocr_motion_comp_reset(ocr_motion_comp_t *mc)
{
    if (!mc) return;
    mc->has_prev = 0;
    memset(&mc->filtered, 0, sizeof(mc->filtered));
    mc->filtered.scale = 1.0f;
}
