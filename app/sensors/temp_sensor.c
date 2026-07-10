/**
 * @file temp_sensor.c
 * @brief ADT7410 温度读取实现
 */
#include "temp_sensor.h"
#include "log.h"

#include <string.h>
#include <stdio.h>

int ocr_temp_sensor_init(ocr_temp_sensor_t *sensor, const char *sysfs_path)
{
    if (!sensor || !sysfs_path) return -1;
    memset(sensor, 0, sizeof(*sensor));
    strncpy(sensor->sysfs_path, sysfs_path, sizeof(sensor->sysfs_path) - 1);
    return 0;
}

int ocr_temp_sensor_read(ocr_temp_sensor_t *sensor, int *temp_mdeg)
{
    if (!sensor || !temp_mdeg) return -1;

    char path[256];
    snprintf(path, sizeof(path), "%s/in_temp_input", sensor->sysfs_path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_W("读取温度失败: %s", path);
        return -2;
    }
    int val = 0;
    if (fscanf(fp, "%d", &val) != 1) {
        fclose(fp);
        return -3;
    }
    fclose(fp);

    /* IIO 温度通常以毫摄氏度为单位 */
    *temp_mdeg = val;
    sensor->cached_temp = val;
    return 0;
}
