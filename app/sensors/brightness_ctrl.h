#ifndef OCR_SENSORS_BRIGHTNESS_CTRL_H
#define OCR_SENSORS_BRIGHTNESS_CTRL_H
/**
 * @file brightness_ctrl.h
 * @brief 亮度调节：分段映射+平滑+回差→/sys/class/backlight
 *
 * 把环境光 lux 映射为背光等级，结合回差和低通滤波减少亮度抖动。
 * 通过 backlight sysfs 写 brightness，实际最大值优先读取 max_brightness，写入失败时更新路径恢复旧值。
 */

#include "light_sensor.h"

#define BRIGHTNESS_MAX 255

/** 亮度控制上下文 */
typedef struct {
    char  sysfs_path[128]; /**< 背光设备目录，初始化剥除可选 /brightness 后缀，后续拼接各属性。 */
    int   cur_level; /**< 软件记录的当前亮度等级，0..max_level；不是 lux 或百分比。 */
    int   target_level; /**< 环境光映射并经回差处理后的目标等级，平滑过程逐步接近它。 */
    int   max_level; /**< 驱动 max_brightness 值，读取失败回退 BRIGHTNESS_MAX，作为等级上限。 */
    int   hysteresis; /**< 目标变化的回差门槛，单位为亮度等级，不是 lux；默认 10。 */
    float smooth_alpha; /**< 一阶低通跟随系数，无量纲，默认 0.3；越大越快接近目标。 */
} ocr_brightness_t;

/**
 * @brief 初始化亮度控制
 * @param[in] ctrl       亮度控制上下文
 * @param[in] sysfs_path 背光 sysfs 路径
 * @return 0=成功，负数=错误
 */
int ocr_brightness_init(ocr_brightness_t *ctrl, const char *sysfs_path);

/**
 * @brief 根据环境光更新背光亮度
 * @param[in] ctrl 亮度控制
 * @param[in] lux  环境光（lux）
 * @return 0=成功，负数=错误
 */
int ocr_brightness_update(ocr_brightness_t *ctrl, int lux);

/**
 * @brief 直接设置亮度等级
 */
int ocr_brightness_set(ocr_brightness_t *ctrl, int level);

/**
 * @brief 获取当前亮度
 */
int ocr_brightness_get(ocr_brightness_t *ctrl);

#endif /* OCR_SENSORS_BRIGHTNESS_CTRL_H */
