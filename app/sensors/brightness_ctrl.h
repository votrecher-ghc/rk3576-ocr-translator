#ifndef OCR_SENSORS_BRIGHTNESS_CTRL_H
#define OCR_SENSORS_BRIGHTNESS_CTRL_H
/**
 * @file brightness_ctrl.h
 * @brief 亮度调节：分段映射+平滑+回差→/sys/class/backlight
 */

#include "light_sensor.h"

#define BRIGHTNESS_MAX 255

/** 亮度控制上下文 */
typedef struct {
    char  sysfs_path[128];  /* 背光 sysfs 路径 */
    int   cur_level;        /* 当前亮度等级 (0~max) */
    int   target_level;     /* 目标亮度等级 */
    int   max_level;        /* 最大亮度等级 */
    int   hysteresis;       /* 回差阈值 */
    float smooth_alpha;     /* 平滑系数 */
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
