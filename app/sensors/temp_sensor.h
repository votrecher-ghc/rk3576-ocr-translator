#ifndef OCR_SENSORS_TEMP_SENSOR_H
#define OCR_SENSORS_TEMP_SENSOR_H
/**
 * @file temp_sensor.h
 * @brief ADT7410 温度读取（IIO sysfs）
 *
 * 读取 IIO 温度属性，优先使用处理值，必要时根据 raw/offset/scale 换算。
 * 对外统一采用毫摄氏度，风扇温度阈值必须使用相同单位。
 */

#include <stdint.h>

/** 温度传感器上下文 */
typedef struct {
    char sysfs_path[128]; /**< 温度传感器 IIO sysfs 设备目录，含 NUL；不是 /dev/iio 字符设备路径。 */
    int  cached_temp; /**< 最近一次成功读取的温度，毫摄氏度；例如 25000 表示 25°C。 */
} ocr_temp_sensor_t;

/**
 * @brief 初始化温度传感器
 */
int ocr_temp_sensor_init(ocr_temp_sensor_t *sensor, const char *sysfs_path);

/**
 * @brief 读取温度
 * @param[out] temp_mdeg 温度（毫摄氏度，如 35000 = 35.0°C）
 * @return 0=成功，负数=错误
 */
int ocr_temp_sensor_read(ocr_temp_sensor_t *sensor, int *temp_mdeg);

#endif /* OCR_SENSORS_TEMP_SENSOR_H */
