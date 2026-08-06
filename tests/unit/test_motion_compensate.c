#include "motion_compensate.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static quaternion_t rotation_y_degrees(float degrees)
{
    float half = degrees * (float)M_PI / 360.0f;
    quaternion_t q = {cosf(half), 0.0f, sinf(half), 0.0f};
    return q;
}

int main(void)
{
    ocr_attitude_t attitude;
    ocr_motion_comp_t motion;
    ocr_stab_params_t params;

    assert(ocr_attitude_init(&attitude, 1000.0f, 0.1f) == 0);
    assert(ocr_motion_comp_init(&motion, 60.0f, 45.0f, 0.0f) == 0);
    motion.max_scale = 1.2f;

    /* 第一帧只建立平滑目标姿态，窗口应居中。 */
    assert(ocr_motion_comp_update(&motion, &attitude, 1920, 1080,
                                  &params) == 0);
    assert(fabsf(params.dx) < 1.0e-5f);
    assert(fabsf(params.dy) < 1.0e-5f);
    assert(fabsf(params.scale - 1.2f) < 1.0e-5f);

    /* 绕摄像头 Y 轴正向旋转，应只产生水平补偿。 */
    attitude.q = rotation_y_degrees(5.0f);
    assert(ocr_motion_comp_update(&motion, &attitude, 1920, 1080,
                                  &params) == 0);
    assert(params.dx > 140.0f && params.dx < 150.0f);
    assert(fabsf(params.dy) < 1.0e-3f);
    assert(params.angle == 0.0f);

    puts("motion compensation tests passed");
    return 0;
}
