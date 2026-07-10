#ifndef OCR_STAB_IMU_READER_H
#define OCR_STAB_IMU_READER_H
/**
 * @file imu_reader.h
 * @brief IMU 读取：IIO 设备（/dev/iio:deviceX, triggered_buffer）
 *
 * 从 IIO triggered buffer 读取 6/9 轴 IMU 数据（加速度+陀螺仪+磁力计）。
 */

#include <stdint.h>
#include <stdatomic.h>
#include "ringbuffer.h"
#include "thread.h"

/** IMU 原始数据样本 */
typedef struct {
    int16_t accel[3];   /* 加速度 x/y/z（原始值） */
    int16_t gyro[3];    /* 陀螺仪 x/y/z（原始值） */
    int16_t mag[3];     /* 磁力计 x/y/z（原始值，可选） */
    int64_t timestamp;  /* 时间戳（纳秒） */
} imu_sample_t;

/** IMU 读取上下文 */
typedef struct {
    int           dev_fd;        /* /dev/iio:deviceX fd */
    char          dev_name[64];  /* 设备节点路径 */
    int           accel_scale;   /* 加速度缩放（微 g/LSB） */
    int           gyro_scale;    /* 陀螺仪缩放（mdps/LSB） */
    ocr_ringbuffer_t ring;      /* 采样数据环形缓冲 */
    atomic_int    running;       /* 读取线程运行标志 */
    ocr_thread_t  thread;        /* 读取线程 */
    uint64_t      sample_count;  /* 采样计数 */
} ocr_imu_reader_t;

/**
 * @brief 初始化 IMU 读取
 * @param[in] reader   读取上下文
 * @param[in] dev_path IIO 设备路径（如 /dev/iio:device0）
 * @param[in] buf_depth 环形缓冲深度
 * @return 0=成功，负数=错误
 */
int ocr_imu_reader_init(ocr_imu_reader_t *reader, const char *dev_path, int buf_depth);

/**
 * @brief 启动 IMU 读取线程
 */
int ocr_imu_reader_start(ocr_imu_reader_t *reader);

/**
 * @brief 停止读取
 */
int ocr_imu_reader_stop(ocr_imu_reader_t *reader);

/**
 * @brief 从环形缓冲取一个采样（非阻塞）
 * @return 0=成功，1=空，负数=错误
 */
int ocr_imu_reader_get(ocr_imu_reader_t *reader, imu_sample_t *sample);

/**
 * @brief 销毁 IMU 读取
 */
void ocr_imu_reader_destroy(ocr_imu_reader_t *reader);

#endif /* OCR_STAB_IMU_READER_H */
