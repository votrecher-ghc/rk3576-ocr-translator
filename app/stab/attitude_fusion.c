/**
 * @file attitude_fusion.c
 * @brief 带数值保护、启动零偏校准和安装矩阵的六轴 Madgwick 融合。
 */
#include "attitude_fusion.h"
#include "math_utils.h"

#include <math.h>
#include <string.h>

#define OCR_ATTITUDE_EPSILON          1.0e-9f
#define OCR_ATTITUDE_MAX_SAMPLE_HZ    100000.0f
#define OCR_ATTITUDE_MAX_STEPS        256
#define OCR_ATTITUDE_GRAVITY          9.80665f
#define OCR_ATTITUDE_ACCEL_TOLERANCE  2.5f
#define OCR_ATTITUDE_STILL_GYRO_MAX   0.15f
#define OCR_ATTITUDE_BIAS_SECONDS     2.0f
#define OCR_ATTITUDE_BIAS_MIN_SAMPLES 200U

static const float identity_mount_matrix[9] = {
    1.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 1.0f,
};

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

static void set_identity_quaternion(ocr_attitude_t *att)
{
    att->q.w = 1.0f;
    att->q.x = 0.0f;
    att->q.y = 0.0f;
    att->q.z = 0.0f;
}

static void clear_bias_calibration(ocr_attitude_t *att)
{
    memset(att->gyro_bias, 0, sizeof(att->gyro_bias));
    memset(att->gyro_bias_sum, 0, sizeof(att->gyro_bias_sum));
    att->gyro_bias_elapsed = 0.0f;
    att->gyro_bias_samples = 0;
    att->gyro_bias_ready = 0;
}

static void transform_vector(const float matrix[9], const float input[3],
                             float output[3])
{
    output[0] = matrix[0] * input[0] + matrix[1] * input[1] +
                matrix[2] * input[2];
    output[1] = matrix[3] * input[0] + matrix[4] * input[1] +
                matrix[5] * input[2];
    output[2] = matrix[6] * input[0] + matrix[7] * input[1] +
                matrix[8] * input[2];
}

static int validate_mount_matrix(const float matrix[9])
{
    float row_norm[3];
    float dot01;
    float dot02;
    float dot12;
    float determinant;

    if (!matrix) return 0;
    for (int i = 0; i < 9; ++i) {
        if (!isfinite(matrix[i])) return -1;
    }

    for (int row = 0; row < 3; ++row) {
        const float *r = &matrix[row * 3];
        row_norm[row] = hypotf(hypotf(r[0], r[1]), r[2]);
        if (row_norm[row] < 0.9f || row_norm[row] > 1.1f) return -1;
    }
    dot01 = matrix[0] * matrix[3] + matrix[1] * matrix[4] +
            matrix[2] * matrix[5];
    dot02 = matrix[0] * matrix[6] + matrix[1] * matrix[7] +
            matrix[2] * matrix[8];
    dot12 = matrix[3] * matrix[6] + matrix[4] * matrix[7] +
            matrix[5] * matrix[8];
    if (fabsf(dot01) > 0.1f || fabsf(dot02) > 0.1f ||
        fabsf(dot12) > 0.1f) {
        return -1;
    }

    determinant =
        matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
        matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
        matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return fabsf(fabsf(determinant) - 1.0f) <= 0.2f ? 0 : -1;
}

int ocr_attitude_init(ocr_attitude_t *att, float sample_hz, float beta)
{
    if (!att || !isfinite(sample_hz) || sample_hz <= 0.0f ||
        sample_hz > OCR_ATTITUDE_MAX_SAMPLE_HZ ||
        !isfinite(beta) || beta < 0.0f) {
        return -1;
    }
    memset(att, 0, sizeof(*att));
    set_identity_quaternion(att);
    memcpy(att->mount_matrix, identity_mount_matrix,
           sizeof(att->mount_matrix));
    att->beta = beta;
    att->sample_dt = 1.0f / sample_hz;
    att->accel_scale = 1.0f;
    att->gyro_scale = 1.0f;
    att->max_dt = 0.25f;
    return 0;
}

int ocr_attitude_set_mount_matrix(ocr_attitude_t *att,
                                  const float matrix[9])
{
    if (!att || validate_mount_matrix(matrix) != 0) return -1;
    memcpy(att->mount_matrix,
           matrix ? matrix : identity_mount_matrix,
           sizeof(att->mount_matrix));
    clear_bias_calibration(att);
    set_identity_quaternion(att);
    return 0;
}

static void update_gyro_bias(ocr_attitude_t *att, const float accel[3],
                             const float gyro[3], float dt)
{
    float accel_norm;
    float gyro_norm;

    if (att->gyro_bias_ready) return;
    accel_norm = hypotf(hypotf(accel[0], accel[1]), accel[2]);
    gyro_norm = hypotf(hypotf(gyro[0], gyro[1]), gyro[2]);

    if (!isfinite(accel_norm) || !isfinite(gyro_norm) ||
        fabsf(accel_norm - OCR_ATTITUDE_GRAVITY) >
            OCR_ATTITUDE_ACCEL_TOLERANCE ||
        gyro_norm > OCR_ATTITUDE_STILL_GYRO_MAX) {
        memset(att->gyro_bias_sum, 0, sizeof(att->gyro_bias_sum));
        att->gyro_bias_elapsed = 0.0f;
        att->gyro_bias_samples = 0;
        return;
    }

    for (int axis = 0; axis < 3; ++axis)
        att->gyro_bias_sum[axis] += gyro[axis];
    att->gyro_bias_elapsed += dt;
    ++att->gyro_bias_samples;

    if (att->gyro_bias_elapsed >= OCR_ATTITUDE_BIAS_SECONDS &&
        att->gyro_bias_samples >= OCR_ATTITUDE_BIAS_MIN_SAMPLES) {
        float count = (float)att->gyro_bias_samples;
        for (int axis = 0; axis < 3; ++axis)
            att->gyro_bias[axis] = att->gyro_bias_sum[axis] / count;
        att->gyro_bias_ready = 1;
    }
}

static void correct_gyro(const ocr_attitude_t *att, const float gyro[3],
                         float corrected[3])
{
    float provisional_bias[3] = {0.0f, 0.0f, 0.0f};

    if (att->gyro_bias_ready) {
        memcpy(provisional_bias, att->gyro_bias, sizeof(provisional_bias));
    } else if (att->gyro_bias_samples > 0) {
        float count = (float)att->gyro_bias_samples;
        for (int axis = 0; axis < 3; ++axis)
            provisional_bias[axis] = att->gyro_bias_sum[axis] / count;
    }

    for (int axis = 0; axis < 3; ++axis)
        corrected[axis] = gyro[axis] - provisional_bias[axis];
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

    /* 无可用重力向量时仍保留纯陀螺仪积分。 */
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
    float accel_camera[3];
    float gyro_camera[3];
    float gyro_corrected[3];
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

    transform_vector(att->mount_matrix, accel, accel_camera);
    transform_vector(att->mount_matrix, gyro, gyro_camera);
    update_gyro_bias(att, accel_camera, gyro_camera, dt);
    correct_gyro(att, gyro_camera, gyro_corrected);

    steps = (int)ceilf(dt / att->sample_dt);
    if (steps < 1) steps = 1;
    if (steps > OCR_ATTITUDE_MAX_STEPS) steps = OCR_ATTITUDE_MAX_STEPS;
    step_dt = dt / (float)steps;
    for (int step = 0; step < steps; ++step) {
        if (attitude_step(att, accel_camera, gyro_corrected, step_dt) != 0) {
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
    /* 磁力计修正尚未进入 EIS 数据路径。 */
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

int ocr_attitude_bias_ready(const ocr_attitude_t *att)
{
    return att && att->gyro_bias_ready;
}

void ocr_attitude_reset(ocr_attitude_t *att)
{
    if (!att) return;
    set_identity_quaternion(att);
}
