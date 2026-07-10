#ifndef OCR_SENSORS_FAN_CTRL_H
#define OCR_SENSORS_FAN_CTRL_H
/**
 * @file fan_ctrl.h
 * @brief 风扇调速：状态机+回差→/sys/class/pwm
 */

#include <stdint.h>

#define FAN_MAX_LEVEL 4

/** 风扇控制上下文 */
typedef struct {
    char   pwm_path[128];   /* PWM sysfs 路径 */
    int    period_ns;       /* PWM 周期（纳秒） */
    int    cur_duty_ns;     /* 当前占空比（纳秒） */
    int    cur_level;       /* 当前等级 (0~FAN_MAX_LEVEL) */
    int    hysteresis_mdeg; /* 回差（毫摄氏度） */
    /* 温度阈值（毫摄氏度），对应各等级切换点 */
    int    threshold_up[FAN_MAX_LEVEL];   /* 升级阈值 */
    int    threshold_down[FAN_MAX_LEVEL]; /* 降级阈值 */
} ocr_fan_ctrl_t;

/**
 * @brief 初始化风扇控制
 * @param[in] ctrl     风扇控制
 * @param[in] pwm_path PWM sysfs 路径
 * @return 0=成功，负数=错误
 */
int ocr_fan_ctrl_init(ocr_fan_ctrl_t *ctrl, const char *pwm_path);

/**
 * @brief 根据温度更新风扇等级
 * @param[in] ctrl      风扇控制
 * @param[in] temp_mdeg 温度（毫摄氏度）
 * @return 0=成功，负数=错误
 */
int ocr_fan_ctrl_update(ocr_fan_ctrl_t *ctrl, int temp_mdeg);

/**
 * @brief 直接设置风扇等级
 * @param[in] ctrl   风扇控制
 * @param[in] level  等级 (0=关, 1~FAN_MAX_LEVEL)
 */
int ocr_fan_ctrl_set_level(ocr_fan_ctrl_t *ctrl, int level);

#endif /* OCR_SENSORS_FAN_CTRL_H */
