/**
 * @file attitude_fusion.c
 * @brief Madgwick 姿态融合实现
 */
#include "attitude_fusion.h"
#include "math_utils.h"
#include "log.h"

#include <string.h>
#include <math.h>

int ocr_attitude_init(ocr_attitude_t *att, float sample_hz, float beta)
{
    if (!att) return -1;
    memset(att, 0, sizeof(*att));
    att->q.w = 1.0f; /* 单位四元数 */
    att->q.x = att->q.y = att->q.z = 0.0f;
    att->beta = beta;
    att->sample_dt = 1.0f / sample_hz;
    att->accel_scale = 9.80665f / 1000.0f; /* 假设原始为 mg */
    att->gyro_scale = (float)M_PI / 180.0f / 1000.0f; /* 假设原始为 mdps */
    return 0;
}

/* 快速倒数平方根（Madgwick 原版算法使用） */
static float inv_sqrt(float x)
{
    return 1.0f / sqrtf(x);
}

int ocr_attitude_update(ocr_attitude_t *att, const float accel[3],
                        const float gyro[3], float dt)
{
    if (!att || !accel || !gyro) return -1;

    float ax = accel[0], ay = accel[1], az = accel[2];
    float gx = gyro[0],  gy = gyro[1],  gz = gyro[2];

    float qw = att->q.w, qx = att->q.x, qy = att->q.y, qz = att->q.z;

    /* 归一化加速度 */
    float norm = inv_sqrt(ax * ax + ay * ay + az * az);
    ax *= norm; ay *= norm; az *= norm;

    /* 梯度下降算法修正（Madgwick） */
    float _2qw = 2.0f * qw;
    float _2qx = 2.0f * qx;
    float _2qy = 2.0f * qy;
    float _2qz = 2.0f * qz;
    float _4qw = 4.0f * qw;
    float _4qx = 4.0f * qx;
    float _4qy = 4.0f * qy;
    float _8qx = 8.0f * qx;
    float _8qy = 8.0f * qy;
    float qwqw = qw * qw, qxqx = qx * qx, qyqy = qy * qy, qzqz = qz * qz;

    /* 误差向量（叉积） */
    float sx = _2qy * (2.0f * qx * qz - _2qy * qw) - _2qz * (2.0f * qx * qy + _2qz * qw) + az;
    float sy = _2qx * (2.0f * qy * qz + _2qx * qw) - _2qw * (2.0f * qx * qz - _2qy * qw) - _2qz * (2.0f * qx * qw + _2qz * qx) - az;
    float sz = _2qw * (2.0f * qx * qy + _2qz * qw) + _2qx * (2.0f * qy * qz + _2qx * qw) - _2qy * (2.0f * qx * qw - _2qz * qx) + ax;

    /* 归一化误差 */
    norm = inv_sqrt(sx * sx + sy * sy + sz * sz);
    sx *= norm; sy *= norm; sz *= norm;

    /* 反馈梯度 */
    float step = att->beta;
    sx *= step; sy *= step; sz *= step;

    /* 陀螺仪漂移修正 */
    gx += sx; gy += sy; gz += sz;

    /* 四元数微分积分 */
    float dqw = (-qx * gx - qy * gy - qz * gz) * 0.5f;
    float dqx = ( qw * gx + qy * gz - qz * gy) * 0.5f;
    float dqy = ( qw * gy - qx * gz + qz * gx) * 0.5f;
    float dqz = ( qw * gz + qx * gy - qy * gx) * 0.5f;

    att->q.w += dqw * dt;
    att->q.x += dqx * dt;
    att->q.y += dqy * dt;
    att->q.z += dqz * dt;

    /* 归一化四元数 */
    norm = inv_sqrt(att->q.w * att->q.w + att->q.x * att->q.x +
                    att->q.y * att->q.y + att->q.z * att->q.z);
    att->q.w *= norm;
    att->q.x *= norm;
    att->q.y *= norm;
    att->q.z *= norm;

    return 0;
}

int ocr_attitude_update_9dof(ocr_attitude_t *att, const float accel[3],
                             const float gyro[3], const float mag[3], float dt)
{
    if (!att || !accel || !gyro || !mag) return -1;
    /* TODO: 实现 9 轴 Madgwick 滤波（加入磁力计修正 yaw 漂移） */
    /* 当前回退到 6 轴 */
    return ocr_attitude_update(att, accel, gyro, dt);
}

void ocr_attitude_to_euler(const ocr_attitude_t *att, euler_t *euler)
{
    if (!att || !euler) return;
    float qw = att->q.w, qx = att->q.x, qy = att->q.y, qz = att->q.z;

    /* roll (x-axis rotation) */
    float sinr = 2.0f * (qw * qx + qy * qz);
    float cosr = 1.0f - 2.0f * (qx * qx + qy * qy);
    euler->roll = ocr_rad2deg(atan2f(sinr, cosr));

    /* pitch (y-axis rotation) */
    float sinp = 2.0f * (qw * qy - qz * qx);
    if (fabsf(sinp) >= 1.0f)
        euler->pitch = ocr_rad2deg(copysignf((float)M_PI / 2.0f, sinp));
    else
        euler->pitch = ocr_rad2deg(asinf(sinp));

    /* yaw (z-axis rotation) */
    float siny = 2.0f * (qw * qz + qx * qy);
    float cosy = 1.0f - 2.0f * (qy * qy + qz * qz);
    euler->yaw = ocr_rad2deg(atan2f(siny, cosy));
}

void ocr_attitude_reset(ocr_attitude_t *att)
{
    if (!att) return;
    att->q.w = 1.0f;
    att->q.x = att->q.y = att->q.z = 0.0f;
}
