/**
 * @file attitude_fusion.c
 * @brief Numerically guarded six-axis Madgwick attitude fusion.
 */
#include "attitude_fusion.h"
#include "math_utils.h"

#include <math.h>
#include <string.h>

#define OCR_ATTITUDE_EPSILON       1.0e-9f
#define OCR_ATTITUDE_MAX_SAMPLE_HZ 100000.0f
#define OCR_ATTITUDE_MAX_STEPS     256

static int quaternion_normalize(quaternion_t *q)
{
    float norm;

    if (!q || !isfinite(q->w) || !isfinite(q->x) ||
        !isfinite(q->y) || !isfinite(q->z)) {
        return -1;
    }
    norm = hypotf(hypotf(q->w, q->x), hypotf(q->y, q->z));
    if (!isfinite(norm) || norm <= OCR_ATTITUDE_EPSILON) return -1;
    q->w /= norm;
    q->x /= norm;
    q->y /= norm;
    q->z /= norm;
    return 0;
}

int ocr_attitude_init(ocr_attitude_t *att, float sample_hz, float beta)
{
    if (!att || !isfinite(sample_hz) || sample_hz <= 0.0f ||
        sample_hz > OCR_ATTITUDE_MAX_SAMPLE_HZ ||
        !isfinite(beta) || beta < 0.0f) {
        return -1;
    }
    memset(att, 0, sizeof(*att));
    att->q.w = 1.0f;
    att->beta = beta;
    att->sample_dt = 1.0f / sample_hz;
    att->accel_scale = 1.0f;
    att->gyro_scale = 1.0f;
    att->max_dt = 0.25f;
    return 0;
}

static int attitude_step(ocr_attitude_t *att, const float accel[3],
                         const float gyro[3], float dt)
{
    float q0 = att->q.w;
    float q1 = att->q.x;
    float q2 = att->q.y;
    float q3 = att->q.z;
    float gx = gyro[0];
    float gy = gyro[1];
    float gz = gyro[2];
    float q_dot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    float q_dot1 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    float q_dot2 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    float q_dot3 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);
    float accel_norm = hypotf(hypotf(accel[0], accel[1]), accel[2]);

    /* With no usable gravity vector, gyro integration remains well-defined. */
    if (isfinite(accel_norm) && accel_norm > OCR_ATTITUDE_EPSILON) {
        float ax = accel[0] / accel_norm;
        float ay = accel[1] / accel_norm;
        float az = accel[2] / accel_norm;
        float _2q0 = 2.0f * q0;
        float _2q1 = 2.0f * q1;
        float _2q2 = 2.0f * q2;
        float _2q3 = 2.0f * q3;
        float _4q0 = 4.0f * q0;
        float _4q1 = 4.0f * q1;
        float _4q2 = 4.0f * q2;
        float _8q1 = 8.0f * q1;
        float _8q2 = 8.0f * q2;
        float q0q0 = q0 * q0;
        float q1q1 = q1 * q1;
        float q2q2 = q2 * q2;
        float q3q3 = q3 * q3;
        float s0 = _4q0 * q2q2 + _2q2 * ax +
                   _4q0 * q1q1 - _2q1 * ay;
        float s1 = _4q1 * q3q3 - _2q3 * ax +
                   4.0f * q0q0 * q1 - _2q0 * ay - _4q1 +
                   _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
        float s2 = 4.0f * q0q0 * q2 + _2q0 * ax +
                   _4q2 * q3q3 - _2q3 * ay - _4q2 +
                   _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
        float s3 = 4.0f * q1q1 * q3 - _2q1 * ax +
                   4.0f * q2q2 * q3 - _2q2 * ay;
        float step_norm = hypotf(hypotf(s0, s1), hypotf(s2, s3));

        if (isfinite(step_norm) && step_norm > OCR_ATTITUDE_EPSILON) {
            float gain = att->beta / step_norm;
            q_dot0 -= gain * s0;
            q_dot1 -= gain * s1;
            q_dot2 -= gain * s2;
            q_dot3 -= gain * s3;
        }
    }

    att->q.w = q0 + q_dot0 * dt;
    att->q.x = q1 + q_dot1 * dt;
    att->q.y = q2 + q_dot2 * dt;
    att->q.z = q3 + q_dot3 * dt;
    return quaternion_normalize(&att->q);
}

int ocr_attitude_update(ocr_attitude_t *att, const float accel[3],
                        const float gyro[3], float dt)
{
    int steps;
    float step_dt;

    if (!att || !accel || !gyro) return -1;
    for (int axis = 0; axis < 3; ++axis) {
        if (!isfinite(accel[axis]) || !isfinite(gyro[axis])) return -1;
    }
    if (!isfinite(dt) || dt < 0.0f) return -2;
    if (dt == 0.0f) dt = att->sample_dt;
    if (!isfinite(att->sample_dt) || att->sample_dt <= 0.0f ||
        !isfinite(att->max_dt) || att->max_dt <= 0.0f || dt > att->max_dt) {
        return -2;
    }
    if (quaternion_normalize(&att->q) != 0) {
        ocr_attitude_reset(att);
        return -3;
    }

    steps = (int)ceilf(dt / att->sample_dt);
    if (steps < 1) steps = 1;
    if (steps > OCR_ATTITUDE_MAX_STEPS) steps = OCR_ATTITUDE_MAX_STEPS;
    step_dt = dt / (float)steps;
    for (int step = 0; step < steps; ++step) {
        if (attitude_step(att, accel, gyro, step_dt) != 0) {
            ocr_attitude_reset(att);
            return -3;
        }
    }
    return 0;
}

int ocr_attitude_update_9dof(ocr_attitude_t *att, const float accel[3],
                             const float gyro[3], const float mag[3], float dt)
{
    if (!att || !accel || !gyro || !mag) return -1;
    /* Magnetometer correction is not yet part of the EIS data path. */
    return ocr_attitude_update(att, accel, gyro, dt);
}

void ocr_attitude_to_euler(const ocr_attitude_t *att, euler_t *euler)
{
    quaternion_t q;
    float sinr;
    float cosr;
    float sinp;
    float siny;
    float cosy;

    if (!att || !euler) return;
    q = att->q;
    if (quaternion_normalize(&q) != 0) {
        memset(euler, 0, sizeof(*euler));
        return;
    }

    sinr = 2.0f * (q.w * q.x + q.y * q.z);
    cosr = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    euler->roll = ocr_rad2deg(atan2f(sinr, cosr));

    sinp = 2.0f * (q.w * q.y - q.z * q.x);
    sinp = ocr_clamp_f(sinp, -1.0f, 1.0f);
    euler->pitch = ocr_rad2deg(asinf(sinp));

    siny = 2.0f * (q.w * q.z + q.x * q.y);
    cosy = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    euler->yaw = ocr_rad2deg(atan2f(siny, cosy));
}

void ocr_attitude_reset(ocr_attitude_t *att)
{
    if (!att) return;
    att->q.w = 1.0f;
    att->q.x = 0.0f;
    att->q.y = 0.0f;
    att->q.z = 0.0f;
}
