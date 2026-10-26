#ifndef OCR_STAB_ATTITUDE_FUSION_H
#define OCR_STAB_ATTITUDE_FUSION_H
/**
 * @file attitude_fusion.h
 * @brief 带启动零偏校准和安装矩阵的 Madgwick 姿态融合
 *
 * 六轴姿态融合：把 IMU 轴转换到摄像头坐标系，估计启动静止零偏，再积分更新四元数。
 * 加速度使用 m/s²，角速度使用 rad/s，dt 使用秒；欧拉角输出使用度，异常大时间间隔会被拒绝。
 */

#include "imu_reader.h"

#include <stdint.h>

/** 四元数 */
typedef struct {
    float w, /**< 单位姿态四元数的标量部分，无量纲；初始化单位姿态为 1。 */
          x, /**< 四元数的 X 向量分量，无量纲；不是欧拉角 roll 或角速度。 */
          y, /**< 四元数的 Y 向量分量，无量纲；与 w/x/z 共同表示旋转。 */
          z; /**< 四元数的 Z 向量分量，无量纲；正常融合/插值后四分量平方和为 1。 */
} quaternion_t;

/** 欧拉角（度） */
typedef struct {
    float roll; /**< 绕摄像头 X 轴的欧拉角，度；当前坐标约定 X 向右。 */
    float pitch; /**< 绕摄像头 Y 轴的欧拉角，度；当前坐标约定 Y 向下。 */
    float yaw; /**< 绕摄像头 Z 轴的欧拉角，度；当前坐标约定 Z 向前，不等同于绝对地理航向。 */
} euler_t;

/** 姿态融合上下文 */
typedef struct {
    quaternion_t q; /**< 当前融合单位四元数，积分更新后归一化；由姿态层串行读写。 */
    float beta; /**< Madgwick 重力梯度修正增益，越大越快校正倾斜；不是目标轨迹低通 alpha。 */
    float sample_dt; /**< 名义采样周期，秒，等于 1/sample_hz；dt=0 时回退使用，并用于划分积分小步。 */
    float accel_scale; /**< 兼容字段，初始化为 1；update 已接收 m/s²，不再乘此字段。 */
    float gyro_scale; /**< 兼容字段，初始化为 1；update 已接收 rad/s，不再乘此字段。 */
    float max_dt; /**< 允许一次更新跨越的最大秒数，默认 0.25；超过则拒绝本次积分。 */

    /* 传感器坐标到摄像头坐标的 3x3 行主序矩阵，默认单位阵。 */
    float mount_matrix[9]; /**< 传感器轴到摄像头轴的 3x3 行主序旋转矩阵，默认单位阵。 */

    /* 启动静止阶段估计的陀螺仪零偏，单位 rad/s。 */
    float gyro_bias[3]; /**< 已完成启动静止校准的三轴角速度零偏，rad/s，在摄像头坐标系中扣除。 */
    float gyro_bias_sum[3]; /**< 当前连续静止窗口三轴角速度的逐样本累计和；除 samples 求平均，未乘 dt。 */
    float gyro_bias_elapsed; /**< 当前连续静止窗口累计时长，秒；检测到运动后清零。 */
    uint32_t gyro_bias_samples; /**< 当前静止窗口累计样本数，需与时长门槛同时满足才完成校准。 */
    int gyro_bias_ready; /**< 启动零偏已确定为 1；后续使用 gyro_bias，reset 保留已完成校准。 */
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
 * @return 0=成功，-1=上下文或矩阵无效
 * @note 设置成功会重置姿态并清除启动零偏校准，随后需要重新保持静止完成校准。
 */
int ocr_attitude_set_mount_matrix(ocr_attitude_t *att,
                                  const float matrix[9]);

/**
 * @brief 更新姿态（6 轴：加速度+陀螺仪）
 * @param[in] att    姿态上下文
 * @param[in] accel  加速度（m/s^2，传感器坐标系）
 * @param[in] gyro   陀螺仪（rad/s，传感器坐标系）
 * @param[in] dt     时间步长（秒）；0 使用初始化时的 sample_dt
 * @return 0=成功，-1=传感器数据无效，-2=时间步长无效或过大，-3=姿态数值异常
 * @note 数值异常会重置姿态到单位四元数；同一上下文的更新与读取须由调用者串行化。
 */
int ocr_attitude_update(ocr_attitude_t *att, const float accel[3],
                        const float gyro[3], float dt);

/**
 * @brief 九轴兼容入口，当前委托六轴更新，磁力计数值不参与融合
 * @param[in] mag 必须非 NULL 的兼容参数，当前不读取其数值
 * @return 与 ocr_attitude_update 相同；mag 为 NULL 时返回 -1
 * @note accel/gyro/dt 的单位和约束同六轴接口；本接口不提供绝对航向校正。
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
