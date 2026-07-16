#ifndef OCR_STAB_IMU_READER_H
#define OCR_STAB_IMU_READER_H
/**
 * @file imu_reader.h
 * @brief IMU 读取：IIO 设备（/dev/iio:deviceX, triggered_buffer）
 *
 * 从 IIO triggered buffer 读取 6/9 轴 IMU 数据（加速度+陀螺仪+磁力计）。
 */

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "ringbuffer.h"
#include "thread.h"

#define OCR_IMU_AXIS_COUNT         3
#define OCR_IMU_MAX_SCAN_CHANNELS 32
#define OCR_IMU_SCAN_NAME_LEN     64
#define OCR_IMU_SYSFS_PATH_LEN   256

/** IMU 原始数据样本 */
typedef struct {
    int16_t accel[3];   /* 加速度 x/y/z（原始值） */
    int16_t gyro[3];    /* 陀螺仪 x/y/z（原始值） */
    int16_t mag[3];     /* 磁力计 x/y/z（原始值，可选） */
    int64_t timestamp;  /* 时间戳（纳秒） */
    int64_t accel_raw[3]; /* 不截断的 IIO 原始加速度值 */
    int64_t gyro_raw[3];  /* 不截断的 IIO 原始角速度值 */
    float   accel_si[3];  /* scale 后加速度（m/s^2） */
    float   gyro_si[3];   /* scale 后角速度（rad/s） */
} imu_sample_t;

typedef enum {
    OCR_IMU_SCAN_OTHER = 0,
    OCR_IMU_SCAN_ACCEL,
    OCR_IMU_SCAN_GYRO,
    OCR_IMU_SCAN_TIMESTAMP,
} ocr_imu_scan_kind_t;

/** 从 scan_elements 发现的单个已启用 IIO 通道。 */
typedef struct {
    char                    name[OCR_IMU_SCAN_NAME_LEN];
    int                     scan_index;
    size_t                  offset;
    uint8_t                 storage_bytes;
    uint8_t                 real_bits;
    uint8_t                 shift;
    uint8_t                 is_signed;
    uint8_t                 is_big_endian;
    int8_t                  axis;  /* accel/gyro: 0=x, 1=y, 2=z */
    ocr_imu_scan_kind_t     kind;
    double                  scale; /* IIO ABI SI unit/LSB */
} ocr_imu_scan_channel_t;

/** IMU 读取上下文 */
typedef struct {
    int           dev_fd;        /* /dev/iio:deviceX fd */
    int           lock_fd;       /* 持有 /run 下的设备所有权锁 */
    char          dev_name[64];  /* 设备节点路径 */
    int           accel_scale;   /* 兼容字段：首轴 scale * 1e6 */
    int           gyro_scale;    /* 兼容字段：首轴 scale * 1e6 */
    ocr_ringbuffer_t ring;      /* 采样数据环形缓冲 */
    atomic_int    running;       /* 读取线程运行标志 */
    ocr_thread_t  thread;        /* 读取线程 */
    uint64_t      sample_count;  /* 采样计数 */
    uint64_t      dropped_count; /* 环形缓冲已满而丢弃的样本数 */
    uint64_t      parse_error_count;
    char          sysfs_path[OCR_IMU_SYSFS_PATH_LEN];
    double        accel_scale_si[OCR_IMU_AXIS_COUNT];
    double        gyro_scale_si[OCR_IMU_AXIS_COUNT];
    size_t        scan_bytes;
    int           scan_channel_count;
    ocr_imu_scan_channel_t scan_channels[OCR_IMU_MAX_SCAN_CHANNELS];
    int           thread_started;
    ocr_mutex_t   buffer_lock;   /* 串行化 sysfs buffer 状态切换 */
    int           buffer_lock_initialized;
    int           buffer_enabled;
    int           initialized;
} ocr_imu_reader_t;

/**
 * @brief 初始化 IMU 读取
 * @param[in] reader   读取上下文
 * @param[in] dev_path 动态发现的 IIO 设备路径（如 /dev/iio:deviceN）
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
