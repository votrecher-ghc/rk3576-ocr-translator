/**
 * @file motion_compensate.c
 * @brief 平滑目标姿态到固定裁剪窗口偏移的转换。
 *
 * 平滑虚拟摄像机姿态与当前姿态之差，通过视场角换算成像素位移，限制在裁剪余量内。
 * scale 保持固定以避免画面呼吸；当前仅输出平移补偿，绕光轴的小角度旋转不进入 RGA 执行路径。
 */
#include "motion_compensate.h"
#include "math_utils.h"

#include <math.h>
#include <string.h>

#define OCR_MOTION_DEFAULT_MAX_SHIFT_RATIO 0.10f
#define OCR_MOTION_DEFAULT_MAX_ANGLE_DEG   10.0f
#define OCR_MOTION_DEFAULT_MAX_SCALE        1.25f
#define OCR_MOTION_DEG_TO_RAD               0.01745329251994329577f

static float wrap_degrees(float angle)
{
    float wrapped = fmodf(angle + 180.0f, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    return wrapped - 180.0f;
}

/** @brief 先把角度差折回最短方向，再低通跟随，防止跨越 ±180 度时产生大幅跳变。 */
static float follow_angle(float smooth, float current, float alpha)
{
    float error = wrap_degrees(current - smooth);
    return wrap_degrees(smooth + alpha * error);
}

/** @brief 通过视场角和图像尺寸估计像素焦距，把姿态误差换算为图像平移。 */
static float focal_length_pixels(uint32_t image_size, float fov_deg)
{
    float half_fov = 0.5f * fov_deg * OCR_MOTION_DEG_TO_RAD;
    float tangent = tanf(half_fov);

    if (!isfinite(tangent) || tangent <= 0.0f) return 0.0f;
    return 0.5f * (float)image_size / tangent;
}

int ocr_motion_comp_init(ocr_motion_comp_t *mc, float hfov, float vfov,
                         float alpha)
{
    if (!mc || !isfinite(hfov) || !isfinite(vfov) ||
        hfov <= 0.0f || hfov >= 179.0f ||
        vfov <= 0.0f || vfov >= 179.0f ||
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
    return 0;
}

/** @brief 计算真实姿态与平滑姿态的差，再按固定裁剪余量限制水平和垂直补偿。 */
int ocr_motion_comp_update(ocr_motion_comp_t *mc, const ocr_attitude_t *att,
                           uint32_t img_w, uint32_t img_h,
                           ocr_stab_params_t *params)
{
    euler_t current;
    float error_x;
    float error_y;
    float error_z;
    float fx;
    float fy;
    float crop_w;
    float crop_h;
    float crop_margin_x;
    float crop_margin_y;
    float max_dx;
    float max_dy;
    float dx;
    float dy;

    if (!mc || !att || !params || img_w == 0 || img_h == 0 ||
        !isfinite(mc->hfov_deg) || !isfinite(mc->vfov_deg) ||
        mc->hfov_deg <= 0.0f || mc->hfov_deg >= 179.0f ||
        mc->vfov_deg <= 0.0f || mc->vfov_deg >= 179.0f ||
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

    memset(params, 0, sizeof(*params));
    params->scale = mc->max_scale;

    /* 首帧直接把虚拟姿态设为当前姿态，不产生位移；否则默认全零姿态会让启动
     * 时的任意安装角度被误当成突然抖动，画面立即偏向裁剪边缘。 */
    if (!mc->initialized) {
        mc->smooth_euler = current;
        mc->initialized = 1;
        return 0;
    }

    /*
     * smooth_euler 是虚拟摄像机姿态。alpha 越小，它跟随真实姿态越慢，
     * 快速的小幅旋转就会形成更大的反向补偿；持续转动时它会逐渐追上，
     * 让裁剪窗口回到中心。
     */
    mc->smooth_euler.roll = follow_angle(mc->smooth_euler.roll,
                                         current.roll, mc->alpha);
    mc->smooth_euler.pitch = follow_angle(mc->smooth_euler.pitch,
                                          current.pitch, mc->alpha);
    mc->smooth_euler.yaw = follow_angle(mc->smooth_euler.yaw,
                                        current.yaw, mc->alpha);

    /*
     * 安装矩阵输出采用摄像头坐标：X 向右、Y 向下、Z 向前。
     * roll/pitch/yaw 分别代表绕 X/Y/Z 轴的旋转。
     */
    error_x = wrap_degrees(mc->smooth_euler.roll - current.roll);
    error_y = wrap_degrees(mc->smooth_euler.pitch - current.pitch);
    error_z = wrap_degrees(mc->smooth_euler.yaw - current.yaw);

    fx = focal_length_pixels(img_w, mc->hfov_deg);
    fy = focal_length_pixels(img_h, mc->vfov_deg);
    if (fx <= 0.0f || fy <= 0.0f) return -3;

    /* 绕 Y 轴改变水平视线，绕 X 轴改变垂直视线。 */
    dx = -fx * tanf(error_y * OCR_MOTION_DEG_TO_RAD);
    dy = -fy * tanf(error_x * OCR_MOTION_DEG_TO_RAD);
    if (!isfinite(dx) || !isfinite(dy)) return -3;

    crop_w = (float)img_w / mc->max_scale;
    crop_h = (float)img_h / mc->max_scale;
    crop_margin_x = 0.5f * ((float)img_w - crop_w);
    crop_margin_y = 0.5f * ((float)img_h - crop_h);

    /* 两种上限取较小值：配置允许的最大移动，以及固定缩放后真实剩余的边缘。
     * 例如 scale=1 时没有裁剪余量，即使 max_shift_ratio>0 也必须输出零位移。 */
    max_dx = fminf((float)img_w * mc->max_shift_ratio, crop_margin_x);
    max_dy = fminf((float)img_h * mc->max_shift_ratio, crop_margin_y);
    params->dx = ocr_clamp_f(dx, -max_dx, max_dx);
    params->dy = ocr_clamp_f(dy, -max_dy, max_dy);

    /* 绕 Z 轴会造成图像旋转，RK3576 RGA 无法补偿任意小角度。 */
    (void)error_z;
    params->angle = 0.0f;
    return 0;
}

void ocr_motion_comp_reset(ocr_motion_comp_t *mc)
{
    if (!mc) return;
    mc->initialized = 0;
    memset(&mc->smooth_euler, 0, sizeof(mc->smooth_euler));
}
