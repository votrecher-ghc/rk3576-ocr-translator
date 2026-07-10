/**
 * @file fan_ctrl.c
 * @brief 风扇调速实现
 */
#include "fan_ctrl.h"
#include "log.h"

#include <string.h>
#include <stdio.h>

/* 写 sysfs 字符串值 */
static int write_sysfs_str(const char *path, const char *val)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fputs(val, fp);
    fclose(fp);
    return 0;
}

static int write_sysfs_int(const char *path, int val)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", val);
    return write_sysfs_str(path, buf);
}

/* 等级 → 占空比（纳秒），等级 0=关闭 */
static int level_to_duty(int level, int period)
{
    if (level <= 0) return 0;
    if (level >= FAN_MAX_LEVEL) return period;
    /* 线性映射 */
    return period * level / FAN_MAX_LEVEL;
}

int ocr_fan_ctrl_init(ocr_fan_ctrl_t *ctrl, const char *pwm_path)
{
    if (!ctrl || !pwm_path) return -1;
    memset(ctrl, 0, sizeof(*ctrl));
    strncpy(ctrl->pwm_path, pwm_path, sizeof(ctrl->pwm_path) - 1);
    ctrl->period_ns = 40000; /* 40us = 25kHz */
    ctrl->cur_level = 0;
    ctrl->hysteresis_mdeg = 3000; /* 3°C 回差 */

    /* 温度阈值（毫摄氏度）
     * 等级0→1: 45°C, 1→2: 55°C, 2→3: 65°C, 3→4: 75°C
     * 降级阈值加回差 */
    ctrl->threshold_up[0]   = 45000;
    ctrl->threshold_up[1]   = 55000;
    ctrl->threshold_up[2]   = 65000;
    ctrl->threshold_up[3]   = 75000;
    ctrl->threshold_down[0] = 42000;
    ctrl->threshold_down[1] = 52000;
    ctrl->threshold_down[2] = 62000;
    ctrl->threshold_down[3] = 72000;

    /* 启用 PWM 输出 */
    char path[256];
    snprintf(path, sizeof(path), "%s/period", pwm_path);
    write_sysfs_int(path, ctrl->period_ns);
    snprintf(path, sizeof(path), "%s/enable", pwm_path);
    write_sysfs_int(path, 1);

    LOG_I("风扇控制初始化: pwm=%s period=%dns", pwm_path, ctrl->period_ns);
    return 0;
}

int ocr_fan_ctrl_update(ocr_fan_ctrl_t *ctrl, int temp_mdeg)
{
    if (!ctrl) return -1;

    int new_level = ctrl->cur_level;

    /* 状态机：根据温度升降等级 */
    if (new_level < FAN_MAX_LEVEL && temp_mdeg >= ctrl->threshold_up[new_level]) {
        new_level++;
    } else if (new_level > 0 && temp_mdeg < ctrl->threshold_down[new_level - 1]) {
        new_level--;
    }

    if (new_level != ctrl->cur_level) {
        LOG_I("风扇等级变更: %d → %d (temp=%dm°C)", ctrl->cur_level, new_level, temp_mdeg);
        ctrl->cur_level = new_level;
        return ocr_fan_ctrl_set_level(ctrl, new_level);
    }
    return 0;
}

int ocr_fan_ctrl_set_level(ocr_fan_ctrl_t *ctrl, int level)
{
    if (!ctrl) return -1;
    if (level < 0) level = 0;
    if (level > FAN_MAX_LEVEL) level = FAN_MAX_LEVEL;

    ctrl->cur_level = level;
    ctrl->cur_duty_ns = level_to_duty(level, ctrl->period_ns);

    char path[256];
    snprintf(path, sizeof(path), "%s/duty_cycle", ctrl->pwm_path);
    if (write_sysfs_int(path, ctrl->cur_duty_ns) != 0) {
        LOG_W("写入风扇占空比失败: %s", path);
        return -2;
    }
    return 0;
}
