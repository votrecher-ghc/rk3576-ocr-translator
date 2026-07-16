/**
 * @file light_sensor.c
 * @brief AP3216C 环境光读取实现
 */
#include "light_sensor.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>

int ocr_light_sensor_init(ocr_light_sensor_t *sensor, const char *sysfs_path)
{
    struct stat status;
    size_t path_len;

    if (!sensor || !sysfs_path) return -1;
    path_len = strlen(sysfs_path);
    if (path_len == 0 || path_len >= sizeof(sensor->sysfs_path) ||
        stat(sysfs_path, &status) != 0 ||
        !S_ISDIR(status.st_mode)) return -1;
    memset(sensor, 0, sizeof(*sensor));
    memcpy(sensor->sysfs_path, sysfs_path, path_len + 1);
    return 0;
}

/* 读取 sysfs 整型值 */
static int read_sysfs_double(const char *path, double *value)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int ret = fscanf(fp, "%lf", value) == 1 ? 0 : -1;
    if (fclose(fp) != 0) ret = -1;
    return ret;
}

static int read_channel(const char *base, const char *channel, double *value)
{
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", base, channel);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    return read_sysfs_double(path, value);
}

static int read_scaled_channel(const char *base, const char *raw_channel,
                               const char *scale_channel, double *value)
{
    double raw;
    double scale;

    if (read_channel(base, raw_channel, &raw) != 0 ||
        read_channel(base, scale_channel, &scale) != 0) {
        return -1;
    }
    *value = raw * scale;
    return 0;
}

int ocr_light_sensor_read(ocr_light_sensor_t *sensor, int *lux)
{
    if (!sensor || !lux) return -1;

    double value = 0.0;
    if (read_channel(sensor->sysfs_path, "in_illuminance_input", &value) != 0 &&
        read_channel(sensor->sysfs_path, "in_intensity_both_input", &value) != 0) {
        if (read_scaled_channel(sensor->sysfs_path, "in_illuminance_raw",
                                "in_illuminance_scale", &value) != 0 &&
            read_scaled_channel(sensor->sysfs_path, "in_intensity_both_raw",
                                "in_intensity_scale", &value) != 0) {
            LOG_W("读取环境光失败: %s", sensor->sysfs_path);
            return -2;
        }
    }
    if (!isfinite(value) || value < 0.0 || value > (double)INT32_MAX) {
        LOG_W("读取环境光失败: %s", sensor->sysfs_path);
        return -3;
    }

    *lux = (int)lround(value);
    sensor->cached_lux = *lux;
    return 0;
}
