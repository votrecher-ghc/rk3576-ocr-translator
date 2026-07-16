/**
 * @file motion_compensate.c
 * @brief Bounded, wrap-aware attitude-to-image compensation.
 */
#include "motion_compensate.h"
#include "math_utils.h"

#include <math.h>
#include <string.h>

#define OCR_MOTION_DEFAULT_MAX_SHIFT_RATIO 0.10f
#define OCR_MOTION_DEFAULT_MAX_ANGLE_DEG   10.0f
#define OCR_MOTION_DEFAULT_MAX_SCALE        1.25f

static float wrap_degrees(float angle)
{
    float wrapped = fmodf(angle + 180.0f, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    return wrapped - 180.0f;
}

int ocr_motion_comp_init(ocr_motion_comp_t *mc, float hfov, float vfov,
                         float alpha)
{
    if (!mc || !isfinite(hfov) || !isfinite(vfov) ||
        hfov <= 0.0f || vfov <= 0.0f ||
        !isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return -1;
    }
    memset(mc, 0, sizeof(*mc));
    mc->hfov_deg = hfov;
    mc->vfov_deg = vfov;
    mc->alpha = alpha;
    mc->max_shift_ratio = OCR_MOTION_DEFAULT_MAX_SHIFT_RATIO;
    mc->max_angle_deg = OCR_MOTION_DEFAULT_MAX_ANGLE_DEG;
    mc->max_scale = OCR_MOTION_DEFAULT_MAX_SCALE;
    mc->filtered.scale = 1.0f;
    return 0;
}

int ocr_motion_comp_update(ocr_motion_comp_t *mc, const ocr_attitude_t *att,
                           uint32_t img_w, uint32_t img_h,
                           ocr_stab_params_t *params)
{
    euler_t current;
    float delta_pitch;
    float delta_roll;
    float delta_yaw;
    float effective_shift_ratio;
    float max_dx;
    float max_dy;
    float dx;
    float dy;
    float angle;
    float translation_ratio;
    float target_scale;

    if (!mc || !att || !params || img_w == 0 || img_h == 0 ||
        !isfinite(mc->hfov_deg) || !isfinite(mc->vfov_deg) ||
        mc->hfov_deg <= 0.0f || mc->vfov_deg <= 0.0f ||
        !isfinite(mc->alpha) || mc->alpha < 0.0f || mc->alpha > 1.0f ||
        !isfinite(mc->max_shift_ratio) || mc->max_shift_ratio < 0.0f ||
        mc->max_shift_ratio >= 0.5f ||
        !isfinite(mc->max_angle_deg) || mc->max_angle_deg < 0.0f ||
        !isfinite(mc->max_scale) || mc->max_scale < 1.0f) {
        return -1;
    }

    ocr_attitude_to_euler(att, &current);
    if (!isfinite(current.roll) || !isfinite(current.pitch) ||
        !isfinite(current.yaw)) {
        return -2;
    }

    if (!mc->has_prev) {
        mc->prev_euler = current;
        mc->has_prev = 1;
        memset(params, 0, sizeof(*params));
        params->scale = 1.0f;
        mc->filtered = *params;
        return 0;
    }

    delta_pitch = wrap_degrees(current.pitch - mc->prev_euler.pitch);
    delta_roll = wrap_degrees(current.roll - mc->prev_euler.roll);
    delta_yaw = wrap_degrees(current.yaw - mc->prev_euler.yaw);

    effective_shift_ratio = fminf(mc->max_shift_ratio,
                                  0.5f * (1.0f - 1.0f / mc->max_scale));
    max_dx = (float)img_w * effective_shift_ratio;
    max_dy = (float)img_h * effective_shift_ratio;
    dx = -delta_yaw * (float)img_w / mc->hfov_deg;
    dy = -delta_pitch * (float)img_h / mc->vfov_deg;
    angle = -delta_roll;
    dx = ocr_clamp_f(dx, -max_dx, max_dx);
    dy = ocr_clamp_f(dy, -max_dy, max_dy);
    angle = ocr_clamp_f(angle, -mc->max_angle_deg, mc->max_angle_deg);

    mc->filtered.dx = ocr_lowpass(mc->filtered.dx, dx, mc->alpha);
    mc->filtered.dy = ocr_lowpass(mc->filtered.dy, dy, mc->alpha);
    mc->filtered.angle = ocr_lowpass(mc->filtered.angle, angle, mc->alpha);
    mc->filtered.dx = ocr_clamp_f(mc->filtered.dx, -max_dx, max_dx);
    mc->filtered.dy = ocr_clamp_f(mc->filtered.dy, -max_dy, max_dy);
    mc->filtered.angle = ocr_clamp_f(mc->filtered.angle,
                                     -mc->max_angle_deg,
                                     mc->max_angle_deg);

    translation_ratio = fmaxf(fabsf(mc->filtered.dx) / (float)img_w,
                              fabsf(mc->filtered.dy) / (float)img_h);
    if (translation_ratio >= 0.5f) {
        target_scale = mc->max_scale;
    } else {
        target_scale = 1.0f / (1.0f - 2.0f * translation_ratio);
    }
    target_scale = ocr_clamp_f(target_scale, 1.0f, mc->max_scale);
    if (target_scale > mc->filtered.scale) {
        /* Grow immediately so the filtered translation cannot expose an edge. */
        mc->filtered.scale = target_scale;
    } else {
        mc->filtered.scale = ocr_lowpass(mc->filtered.scale, target_scale,
                                         mc->alpha);
    }
    mc->filtered.scale = ocr_clamp_f(mc->filtered.scale, 1.0f,
                                     mc->max_scale);

    *params = mc->filtered;
    mc->prev_euler = current;
    return 0;
}

void ocr_motion_comp_reset(ocr_motion_comp_t *mc)
{
    if (!mc) return;
    mc->has_prev = 0;
    memset(&mc->prev_euler, 0, sizeof(mc->prev_euler));
    memset(&mc->filtered, 0, sizeof(mc->filtered));
    mc->filtered.scale = 1.0f;
}
