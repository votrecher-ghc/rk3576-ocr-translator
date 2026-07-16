/**
 * @file temp_sensor.c
 * @brief ADT7410 温度读取实现
 */
#include "temp_sensor.h"
#include "log.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>

static int read_value(const char *base, const char *name, double *value)
{
    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s", base, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int ret = fscanf(fp, "%lf", value) == 1 ? 0 : -1;
    if (fclose(fp) != 0) ret = -1;
    return ret;
}

int ocr_temp_sensor_init(ocr_temp_sensor_t *sensor, const char *sysfs_path)
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

int ocr_temp_sensor_read(ocr_temp_sensor_t *sensor, int *temp_mdeg)
{
    if (!sensor || !temp_mdeg) return -1;

    double value = 0.0;
    if (read_value(sensor->sysfs_path, "in_temp_input", &value) != 0 &&
        read_value(sensor->sysfs_path, "temp1_input", &value) != 0) {
        double raw = 0.0, scale = 0.0, offset = 0.0;
        if (read_value(sensor->sysfs_path, "in_temp_raw", &raw) != 0 ||
            read_value(sensor->sysfs_path, "in_temp_scale", &scale) != 0) {
            LOG_W("读取温度失败: %s", sensor->sysfs_path);
            return -2;
        }
        (void)read_value(sensor->sysfs_path, "in_temp_offset", &offset);
        value = (raw + offset) * scale;
    }
    if (!isfinite(value) || value < -100000.0 || value > 200000.0) return -3;
    *temp_mdeg = (int)lround(value);
    sensor->cached_temp = *temp_mdeg;
    return 0;
}
