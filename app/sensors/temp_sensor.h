#ifndef OCR_SENSORS_TEMP_SENSOR_H
#define OCR_SENSORS_TEMP_SENSOR_H
/**
 * @file temp_sensor.h
 * @brief ADT7410 温度读取（IIO sysfs）
 */

#include <stdint.h>

/** 温度传感器上下文 */
typedef struct {
    char sysfs_path[128]; /* IIO sysfs 路径 */
    int  cached_temp;     /* 缓存温度（毫摄氏度） */
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
