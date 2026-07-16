#ifndef OCR_STAB_ATTITUDE_FUSION_H
#define OCR_STAB_ATTITUDE_FUSION_H
/**
 * @file attitude_fusion.h
 * @brief Madgwick 姿态融合：四元数更新→roll/pitch/yaw
 *
 * 基于 6 轴（加速度+陀螺仪）或 9 轴（+磁力计）的 Madgwick 滤波算法，
 * 实时估计设备姿态。
 */

#include "imu_reader.h"

/** 四元数 */
typedef struct {
    float w, x, y, z;
} quaternion_t;

/** 欧拉角（度） */
typedef struct {
    float roll;  /* 翻滚（绕 X 轴） */
    float pitch; /* 俯仰（绕 Y 轴） */
    float yaw;   /* 偏航（绕 Z 轴） */
} euler_t;

/** 姿态融合上下文 */
typedef struct {
    quaternion_t q;       /* 当前四元数 */
    float beta;           /* Madgwick 滤波增益 */
    float sample_dt;      /* 采样间隔（秒） */
    float accel_scale;    /* 兼容字段；update 输入已是 SI，固定为 1 */
    float gyro_scale;     /* 兼容字段；update 输入已是 SI，固定为 1 */
    float max_dt;         /* 可接受的最大单次时间跨度（秒） */
} ocr_attitude_t;

/**
 * @brief 初始化姿态融合
 * @param[in] att       姿态上下文
 * @param[in] sample_hz 采样率（Hz）
 * @param[in] beta      Madgwick 滤波增益（典型 0.1~0.5）
 */
int ocr_attitude_init(ocr_attitude_t *att, float sample_hz, float beta);

/**
 * @brief 更新姿态（6 轴：加速度+陀螺仪）
 * @param[in] att    姿态上下文
 * @param[in] accel  加速度（m/s^2）
 * @param[in] gyro   陀螺仪（rad/s）
 * @param[in] dt     时间步长（秒）；0 使用初始化时的 sample_dt
 */
int ocr_attitude_update(ocr_attitude_t *att, const float accel[3],
                        const float gyro[3], float dt);

/**
 * @brief 更新姿态（9 轴：加速度+陀螺仪+磁力计）
 */
int ocr_attitude_update_9dof(ocr_attitude_t *att, const float accel[3],
                             const float gyro[3], const float mag[3], float dt);

/**
 * @brief 从四元数获取欧拉角
 */
void ocr_attitude_to_euler(const ocr_attitude_t *att, euler_t *euler);

/**
 * @brief 重置姿态到单位四元数
 */
void ocr_attitude_reset(ocr_attitude_t *att);

#endif /* OCR_STAB_ATTITUDE_FUSION_H */
