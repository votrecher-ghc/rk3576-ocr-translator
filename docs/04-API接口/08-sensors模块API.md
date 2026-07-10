# sensors模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v1.1 | 2026-07-10 | 项目组 | 移除摄像头曝光控制，AP3216C仅用于屏幕亮度调节 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 文件列表](#2-文件列表)
- [3. 数据结构](#3-数据结构)
- [4. 函数API](#4-函数api)
- [5. 线程安全](#5-线程安全)
- [6. 错误码](#6-错误码)
- [7. 示例](#7-示例)

---

## 1. 概述

sensors 模块管理环境光（AP3216C）和温度（ADT7410）传感器数据，提供屏幕自适应亮度调节和温度监测功能。摄像头曝光由 IMX415 硬件 AE 自动处理，不参与联动。

## 2. 文件列表

| 文件               | 说明                          |
| ------------------ | ----------------------------- |
| light_sensor.h/c   | AP3216C 环境光采集（IIO sysfs）|
| brightness_ctrl.h/c| 屏幕亮度调节（backlight sysfs）|
| temp_sensor.h/c    | ADT7410 温度采集（IIO sysfs）  |
| fan_ctrl.h/c       | PWM 风扇调速（回差状态机）     |

## 3. 数据结构

```c
typedef struct {
    int lux;           // 环境光强度（lux）
    int brightness;    // 当前屏幕亮度等级
} sensors_light_t;

typedef struct {
    int temp_mc;       // 温度（毫摄氏度）
    int fan_duty;      // 当前风扇占空比(%)
} sensors_thermal_t;
```

## 4. 函数API

### 4.1 环境光与亮度

```c
// 初始化光传感器
int ocr_light_sensor_init(ocr_light_sensor_t *sensor, const char *sysfs_path);

// 读取环境光（lux）
int ocr_light_sensor_read(ocr_light_sensor_t *sensor, int *lux);

// 初始化背光控制
int ocr_brightness_init(ocr_brightness_t *ctrl, const char *sysfs_path);

// 根据环境光更新屏幕亮度
int ocr_brightness_update(ocr_brightness_t *ctrl, int lux);

// 直接设置亮度等级
int ocr_brightness_set(ocr_brightness_t *ctrl, int level);
```

### 4.2 温度与风扇

```c
// 初始化温度传感器
int ocr_temp_sensor_init(ocr_temp_sensor_t *sensor, const char *sysfs_path);

// 读取温度（毫摄氏度）
int ocr_temp_sensor_read(ocr_temp_sensor_t *sensor, int *temp_mc);

// 初始化风扇控制
int ocr_fan_ctrl_init(ocr_fan_ctrl_t *ctrl, const char *pwm_path);

// 根据温度更新风扇转速
int ocr_fan_ctrl_update(ocr_fan_ctrl_t *ctrl, int temp_mc);
```

## 5. 线程安全

- `init` 函数：非线程安全，启动前调用
- `read/update` 函数：线程安全（内部使用 sysfs 原子读写）
- 传感器轮询线程（th_sensor）以 ~1Hz 间隔调用 `read` + `update`

## 6. 错误码

| 错误码        | 说明               |
| ------------- | ------------------ |
| OCR_ERR_NODEV | 传感器设备不存在   |
| OCR_ERR_IO    | sysfs 读取失败     |

## 7. 示例

```c
/* 传感器线程主循环 */
ocr_light_sensor_t light;
ocr_brightness_t   bright;
int lux;

ocr_light_sensor_init(&light, "/sys/bus/iio/devices/iio:device0");
ocr_brightness_init(&bright, "/sys/class/backlight/panel-backlight");

while (running) {
    if (ocr_light_sensor_read(&light, &lux) == 0) {
        ocr_brightness_update(&bright, lux);
    }
    usleep(500000); /* 500ms */
}
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)、[../02-模块详细设计/08-自适应亮度.md](../02-模块详细设计/08-自适应亮度.md)
