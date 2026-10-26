#ifndef OCR_SENSORS_LIGHT_SENSOR_H
#define OCR_SENSORS_LIGHT_SENSOR_H
/**
 * @file light_sensor.h
 * @brief AP3216C 环境光读取（IIO sysfs）
 *
 * 通过 IIO sysfs 接口读取环境光强度（lux）。
 *
 * 读取 IIO 环境光数据，优先使用处理后的照度，必要时以 raw 和 scale 计算。
 * 向上层统一返回整数 lux，屏蔽具体 IIO 设备编号和属性路径差异。
 */

#include <stdint.h>

/** 光传感器上下文 */
typedef struct {
    char sysfs_path[128]; /**< 已验证存在的 IIO sysfs 设备目录，含 NUL；读取时拼接 processed 或 raw/scale。 */
    int  cached_lux; /**< 最近一次成功读取并换算的整数照度，lux；读取失败不更新。 */
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
