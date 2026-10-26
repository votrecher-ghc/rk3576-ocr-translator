#ifndef OCR_STAB_IMU_READER_H
#define OCR_STAB_IMU_READER_H
/**
 * @file imu_reader.h
 * @brief IMU 读取：IIO 设备（/dev/iio:deviceX, triggered_buffer）
 *
 * 从 IIO triggered buffer 读取 6/9 轴 IMU 数据（加速度+陀螺仪+磁力计）。
 *
 * IIO 采样读取：独占设备控制、配置 scan 通道、发现二进制布局，然后在线程中解码完整扫描帧。
 * 读取值按通道 scale 转成 SI 单位，经 SPSC 队列交给姿态融合；停止时先结束读取并禁用 IIO buffer。
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
    int16_t accel[3]; /**< 兼容三轴计数，x/y/z；从原始值饱和到 int16_t，不能直接作为 m/s² 融合。 */
    int16_t gyro[3]; /**< 兼容三轴角速度计数，x/y/z，饱和到 int16_t；融合实际读取 gyro_si。 */
    int16_t mag[3]; /**< 预留三轴磁力计计数；当前 ICM42688 六轴读取路径保持为 0，不参与融合。 */
    int64_t timestamp; /**< IIO 扫描帧时间戳，纳秒；姿态历史要求递增且与图像时钟对齐。 */
    int64_t accel_raw[3]; /**< 不截断的三轴 IIO 原始计数，供诊断/重新换算使用。 */
    int64_t gyro_raw[3]; /**< 不截断的三轴 IIO 角速度原始计数，未乘 scale。 */
    float   accel_si[3]; /**< 各轴 raw*scale 后的加速度，m/s²，仍处于传感器坐标系。 */
    float   gyro_si[3]; /**< 各轴 raw*scale 后的角速度，rad/s；姿态层再通过安装矩阵旋转。 */
} imu_sample_t;

typedef enum {
    OCR_IMU_SCAN_OTHER = 0,
    OCR_IMU_SCAN_ACCEL,
    OCR_IMU_SCAN_GYRO,
    OCR_IMU_SCAN_TIMESTAMP,
} ocr_imu_scan_kind_t;

/** 从 scan_elements 发现的单个已启用 IIO 通道。 */
typedef struct {
    char                    name[OCR_IMU_SCAN_NAME_LEN]; /**< scan_elements 通道基名，含 NUL，例如 in_accel_x，不含 _en/_index/_type。 */
    int                     scan_index; /**< 内核 _index 指定的帧内排序号；不能用 sysfs 目录枚举顺序代替。 */
    size_t                  offset; /**< 通道在一帧二进制 scan 中的起始字节偏移，包含前面字段的对齐填充。 */
    uint8_t                 storage_bytes; /**< 通道实际占用字节数，当前接受 1/2/4/8，同时用于帧内自然对齐。 */
    uint8_t                 real_bits; /**< 有效数据位数，1..64；可能小于 storage_bytes*8。 */
    uint8_t                 shift; /**< 解码存储值后右移的位数，再按 real_bits 掩码截取有效值。 */
    uint8_t                 is_signed; /**< 类型说明为 s 时取 1，按有效最高位符号扩展；u 则为 0。 */
    uint8_t                 is_big_endian; /**< 字节序为大端时取 1、小端取 0；解码逐字节组装，不依赖 CPU 端序。 */
    int8_t                  axis; /**< 加速度/角速度轴号 0=x、1=y、2=z；其他通道为 -1。 */
    ocr_imu_scan_kind_t     kind; /**< 通道业务分类：accel、gyro、timestamp 或其他；温度占布局但当前不写入样本。 */
    double                  scale; /**< 每原始计数对应的 IIO SI 比例；加速度为 m/s²/LSB、角速度为 rad/s/LSB。 */
} ocr_imu_scan_channel_t;

/** IMU 读取上下文 */
typedef struct {
    int           dev_fd; /**< 本读取器打开的 /dev/iio:deviceN fd，线程退出后再关闭；-1 表示未打开。 */
    int           lock_fd; /**< 拥有的 /run 设备独占锁 fd，防止其他进程同时配置 IIO buffer/scan。 */
    char          dev_name[64]; /**< 字符设备路径的值副本，含 NUL；用于日志及关联 sysfs 设备。 */
    int           accel_scale; /**< 兼容整数比例，记录 X 轴 SI scale*1e6；真实解码使用逐通道 double scale。 */
    int           gyro_scale; /**< 兼容整数比例，记录 X 轴 SI scale*1e6；不直接用于姿态积分。 */
    ocr_ringbuffer_t ring; /**< 本读取器拥有的 imu_sample_t SPSC 队列；IMU 线程写，姿态消费线程读。 */
    atomic_int    running; /**< 原子运行请求/状态；异常或 stop 置 0，不表示 thread 已 join。 */
    ocr_thread_t  thread; /**< IMU 采样工作线程句柄，thread_started 时仍需由管理路径 join。 */
    uint64_t      sample_count; /**< 工作线程成功解析的样本累计数，含随后因队列满而丢弃的样本。 */
    uint64_t      dropped_count; /**< 已解析但因应用队列满未能入队的样本累计数。 */
    uint64_t      parse_error_count; /**< 完整帧解码失败累计数；当前遇到解析错误会结束读取线程。 */
    char          sysfs_path[OCR_IMU_SYSFS_PATH_LEN]; /**< 与 dev_fd 对应的 IIO sysfs 目录，用于 scan 类型/比例和 buffer 控制。 */
    double        accel_scale_si[OCR_IMU_AXIS_COUNT]; /**< 发现的三轴加速度比例，m/s²/LSB，按 x/y/z 排列。 */
    double        gyro_scale_si[OCR_IMU_AXIS_COUNT]; /**< 发现的三轴角速度比例，rad/s/LSB，按 x/y/z 排列。 */
    size_t        scan_bytes; /**< 一帧二进制扫描总字节数，含字段间和帧尾对齐；短读累计至此大小才解析。 */
    int           scan_channel_count; /**< 已启用并纳入帧布局的通道数，不只包含六轴，也可含温度和时间戳。 */
    ocr_imu_scan_channel_t scan_channels[OCR_IMU_MAX_SCAN_CHANNELS]; /**< 前 scan_channel_count 项有效，按 scan_index 排序的通道布局与解码规则。 */
    int           thread_started; /**< 普通管理字段，线程已创建且未 join 时为 1；start/stop 由管理方串行调用。 */
    ocr_mutex_t   buffer_lock; /**< 串行化 sysfs buffer/enable 的状态切换，避免 worker 退出与 stop 同时操作。 */
    int           buffer_lock_initialized; /**< buffer_lock 可用标志，用于初始化失败时只销毁已建成的锁。 */
    int           buffer_enabled; /**< 最近确认/写入成功的内核 IIO buffer 启用状态，受 buffer_lock 保护。 */
    int           initialized; /**< 设备、扫描布局、应用队列等准备成功标志；不表示采样线程已启动。 */
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
