#ifndef OCR_STAB_ATTITUDE_FUSION_H
#define OCR_STAB_ATTITUDE_FUSION_H
/**
 * @file attitude_fusion.h
 * @brief 带启动零偏校准和安装矩阵的 Madgwick 姿态融合
 */

#include "imu_reader.h"

#include <stdint.h>

/** 四元数 */
typedef struct {
    float w, x, y, z;
} quaternion_t;

/** 欧拉角（度） */
typedef struct {
    float roll;  /* 翻滚（绕摄像头 X 轴） */
    float pitch; /* 俯仰（绕摄像头 Y 轴） */
    float yaw;   /* 偏航（绕摄像头 Z 轴） */
} euler_t;

/** 姿态融合上下文 */
typedef struct {
    quaternion_t q;       /* 当前四元数 */
    float beta;           /* Madgwick 滤波增益 */
    float sample_dt;      /* 采样间隔（秒） */
    float accel_scale;    /* 兼容字段；update 输入已是 SI，固定为 1 */
    float gyro_scale;     /* 兼容字段；update 输入已是 SI，固定为 1 */
    float max_dt;         /* 可接受的最大单次时间跨度（秒） */

    /* 传感器坐标到摄像头坐标的 3x3 行主序矩阵，默认单位阵。 */
    float mount_matrix[9];

    /* 启动静止阶段估计的陀螺仪零偏，单位 rad/s。 */
    float gyro_bias[3];
    float gyro_bias_sum[3];
    float gyro_bias_elapsed;
    uint32_t gyro_bias_samples;
    int gyro_bias_ready;
} ocr_attitude_t;

/**
 * @brief 初始化姿态融合
 * @param[in] att       姿态上下文
 * @param[in] sample_hz 采样率（Hz）
 * @param[in] beta      Madgwick 滤波增益（典型 0.05~0.2）
 */
int ocr_attitude_init(ocr_attitude_t *att, float sample_hz, float beta);

/**
 * @brief 设置 IMU 传感器坐标到摄像头坐标的安装矩阵
 * @param[in] matrix 3x3 行主序正交矩阵；NULL 恢复单位阵
 */
int ocr_attitude_set_mount_matrix(ocr_attitude_t *att,
                                  const float matrix[9]);

/**
 * @brief 更新姿态（6 轴：加速度+陀螺仪）
 * @param[in] att    姿态上下文
 * @param[in] accel  加速度（m/s^2，传感器坐标系）
 * @param[in] gyro   陀螺仪（rad/s，传感器坐标系）
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
 * @brief 查询启动陀螺仪零偏校准是否完成
 */
int ocr_attitude_bias_ready(const ocr_attitude_t *att);

/**
 * @brief 重置姿态到单位四元数；保留已完成的零偏和安装矩阵
 */
void ocr_attitude_reset(ocr_attitude_t *att);

#endif /* OCR_STAB_ATTITUDE_FUSION_H */
