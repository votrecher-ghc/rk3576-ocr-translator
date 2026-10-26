#ifndef OCR_SENSORS_FAN_CTRL_H
#define OCR_SENSORS_FAN_CTRL_H
/**
 * @file fan_ctrl.h
 * @brief 风扇调速：状态机+回差→/sys/class/pwm
 *
 * 用户态 PWM 风扇控制：识别 PWM 通道、独占控制权、设置周期/占空比，并按温度回差调速。
 * 记录 PWM 的来源和初始状态，退出时只释放自己导出的通道，借用的通道则恢复原配置。
 */

#include <stdint.h>

#define FAN_MAX_LEVEL 4

/** 风扇控制上下文 */
typedef struct {
    char   pwm_path[128]; /**< 规范化 PWM 通道目录，例如 .../pwmchipN/pwm0，后续拼接 duty_cycle 等属性。 */
    char   pwm_chip_path[128]; /**< 该通道父 pwmchip 目录，用于 export/unexport；与通道目录区分。 */
    int    lock_fd; /**< 本控制器拥有的 /run 所有权锁文件 fd；释放控制权时关闭。 */
    int    lock_held; /**< 已取得独占 flock 标志；修改 PWM 或回滚状态前必须持有。 */
    int    auto_mode; /**< pwm_path 配置为 auto 的标志，仅自动选择带板级风扇标记的专用通道。 */
    int    auto_takeover; /**< auto 模式接管上次崩溃遗留导出通道的标志，影响清理责任。 */
    int    pwm_channel; /**< 所选 PWM 通道编号，写入父目录 export/unexport 的整数，不是 pwmchip 编号。 */
    int    exported_by_us; /**< 本控制器承担禁用和 unexport 责任的标志，含自有导出/允许接管的遗留通道。 */
    int    initialized; /**< 初始化全部完成、允许调速时为 1；失败时仍可能有待回滚资源。 */
    int    original_state_valid; /**< 成功保存借用通道初态的标志；退出应恢复，而不是 unexport。 */
    int    original_enable; /**< 接管前通道 enable 值，0/1；original_state_valid 时才有效。 */
    int    original_period_ns; /**< 接管前 PWM 周期，纳秒；清理时按合法写入顺序恢复。 */
    int    original_duty_ns; /**< 接管前有效电平持续时间，纳秒；不得大于原始 period。 */
    int    period_ns; /**< 本控制器采用的 PWM 周期，纳秒；当前默认 40000，即 25 kHz。 */
    int    cur_duty_ns; /**< 最近成功写入的 duty_cycle，纳秒；与百分比/档位编号区分。 */
    int    cur_level; /**< 最近成功设置的风扇档位，0..FAN_MAX_LEVEL；按周期线性映射 duty。 */
    int    hysteresis_mdeg; /**< 兼容的回差记录值，毫摄氏度；当前实际升降判断使用两组独立阈值数组。 */
    /* 温度阈值（毫摄氏度），对应各等级切换点 */
    int    threshold_up[FAN_MAX_LEVEL]; /**< 升档温度门槛，毫摄氏度；索引 i 表示 i 档升至 i+1 档。 */
    int    threshold_down[FAN_MAX_LEVEL]; /**< 降档温度门槛，毫摄氏度；索引 i 表示 i+1 档降至 i 档。 */
} ocr_fan_ctrl_t;

/**
 * @brief 初始化风扇控制
 * @param[in] ctrl     风扇控制
 * @param[in] pwm_path PWM sysfs 路径、duty_cycle 路径，或 "auto"
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

/** Restore an external PWM or disable/unexport one owned by this controller.
 *
 * 释放自有 PWM 或恢复借用 PWM，再释放独占锁。
 */
int ocr_fan_ctrl_destroy(ocr_fan_ctrl_t *ctrl);

#endif /* OCR_SENSORS_FAN_CTRL_H */
