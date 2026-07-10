#ifndef OCR_SENSORS_LIGHT_SENSOR_H
#define OCR_SENSORS_LIGHT_SENSOR_H
/**
 * @file light_sensor.h
 * @brief AP3216C 环境光读取（IIO sysfs）
 *
 * 通过 IIO sysfs 接口读取环境光强度（lux）。
 */

#include <stdint.h>

/** 光传感器上下文 */
typedef struct {
    char sysfs_path[128];  /* IIO sysfs 路径（如 /sys/bus/iio/devices/iio:device0） */
    int  cached_lux;       /* 缓存的光照值 */
} ocr_light_sensor_t;

/**
 * @brief 初始化光传感器
 * @param[in] sensor    传感器上下文
 * @param[in] sysfs_path IIO 设备 sysfs 路径
 * @return 0=成功，负数=错误
 */
int ocr_light_sensor_init(ocr_light_sensor_t *sensor, const char *sysfs_path);

/**
 * @brief 读取环境光强度
 * @param[out] lux 光照值（lux）
 * @return 0=成功，负数=错误
 */
int ocr_light_sensor_read(ocr_light_sensor_t *sensor, int *lux);

#endif /* OCR_SENSORS_LIGHT_SENSOR_H */
