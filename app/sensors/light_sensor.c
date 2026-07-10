/**
 * @file light_sensor.c
 * @brief AP3216C 环境光读取实现
 */
#include "light_sensor.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int ocr_light_sensor_init(ocr_light_sensor_t *sensor, const char *sysfs_path)
{
    if (!sensor || !sysfs_path) return -1;
    memset(sensor, 0, sizeof(*sensor));
    strncpy(sensor->sysfs_path, sysfs_path, sizeof(sensor->sysfs_path) - 1);
    return 0;
}

/* 读取 sysfs 整型值 */
static int read_sysfs_int(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) val = -1;
    fclose(fp);
    return val;
}

int ocr_light_sensor_read(ocr_light_sensor_t *sensor, int *lux)
{
    if (!sensor || !lux) return -1;

    /* AP3216C 光照通道：in_illuminance_input */
    char path[256];
    snprintf(path, sizeof(path), "%s/in_illuminance_input", sensor->sysfs_path);

    int val = read_sysfs_int(path);
    if (val < 0) {
        /* 尝试备用通道名 */
        snprintf(path, sizeof(path), "%s/in_intensity_both_input", sensor->sysfs_path);
        val = read_sysfs_int(path);
    }
    if (val < 0) {
        LOG_W("读取环境光失败: %s", sensor->sysfs_path);
        return -2;
    }

    *lux = val;
    sensor->cached_lux = val;
    return 0;
}
