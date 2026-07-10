/**
 * @file brightness_ctrl.c
 * @brief 亮度调节实现
 */
#include "brightness_ctrl.h"
#include "math_utils.h"
#include "log.h"

#include <string.h>
#include <stdio.h>

/* 写 sysfs 整型值 */
static int write_sysfs_int(const char *path, int val)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d", val);
    fclose(fp);
    return 0;
}

/* 读 sysfs 整型值 */
static int read_sysfs_int(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) val = -1;
    fclose(fp);
    return val;
}

/* 环境光 lux → 目标亮度等级（分段映射） */
static int lux_to_level(int lux, int max_level)
{
    if (lux < 10)   return max_level * 10 / 100;   /* 暗环境 10% */
    if (lux < 50)   return max_level * 25 / 100;   /* 25% */
    if (lux < 200)  return max_level * 50 / 100;   /* 50% */
    if (lux < 1000) return max_level * 75 / 100;   /* 75% */
    return max_level;                               /* 强光 100% */
}

int ocr_brightness_init(ocr_brightness_t *ctrl, const char *sysfs_path)
{
    if (!ctrl || !sysfs_path) return -1;
    memset(ctrl, 0, sizeof(*ctrl));
    strncpy(ctrl->sysfs_path, sysfs_path, sizeof(ctrl->sysfs_path) - 1);
    ctrl->hysteresis = 10;     /* 回差 10 级 */
    ctrl->smooth_alpha = 0.3f; /* 平滑系数 */

    /* 读取最大亮度 */
    char path[256];
    snprintf(path, sizeof(path), "%s/max_brightness", sysfs_path);
    ctrl->max_level = read_sysfs_int(path);
    if (ctrl->max_level <= 0) ctrl->max_level = BRIGHTNESS_MAX;

    /* 读取当前亮度 */
    snprintf(path, sizeof(path), "%s/brightness", sysfs_path);
    ctrl->cur_level = read_sysfs_int(path);
    if (ctrl->cur_level < 0) ctrl->cur_level = ctrl->max_level / 2;

    ctrl->target_level = ctrl->cur_level;
    LOG_I("背光初始化: max=%d cur=%d", ctrl->max_level, ctrl->cur_level);
    return 0;
}

int ocr_brightness_update(ocr_brightness_t *ctrl, int lux)
{
    if (!ctrl) return -1;
    int new_target = lux_to_level(lux, ctrl->max_level);

    /* 回差：目标变化小于阈值时不调整 */
    if (abs(new_target - ctrl->target_level) < ctrl->hysteresis) {
        new_target = ctrl->target_level;
    }
    ctrl->target_level = new_target;

    /* 平滑过渡 */
    ctrl->cur_level = (int)(ocr_lowpass((float)ctrl->cur_level,
                                        (float)ctrl->target_level,
                                        ctrl->smooth_alpha) + 0.5f);
    ctrl->cur_level = ocr_clamp_i(ctrl->cur_level, 0, ctrl->max_level);

    /* 写入背光 */
    char path[256];
    snprintf(path, sizeof(path), "%s/brightness", ctrl->sysfs_path);
    return write_sysfs_int(path, ctrl->cur_level);
}

int ocr_brightness_set(ocr_brightness_t *ctrl, int level)
{
    if (!ctrl) return -1;
    ctrl->cur_level = ocr_clamp_i(level, 0, ctrl->max_level);
    ctrl->target_level = ctrl->cur_level;
    char path[256];
    snprintf(path, sizeof(path), "%s/brightness", ctrl->sysfs_path);
    return write_sysfs_int(path, ctrl->cur_level);
}

int ocr_brightness_get(ocr_brightness_t *ctrl)
{
    return ctrl ? ctrl->cur_level : 0;
}
