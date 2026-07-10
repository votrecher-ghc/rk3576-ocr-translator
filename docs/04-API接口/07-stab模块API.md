# stab模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

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

stab 模块采集 ICM42688 IMU 数据，计算抖动补偿量，输出 RGA 裁剪参数用于电子防抖。

## 2. 文件列表

| 文件          | 说明             |
| ------------- | ---------------- |
| stab.h        | API 声明         |
| stab.c        | EIS 算法实现     |
| imu_reader.c  | IIO 数据读取     |

## 3. 数据结构

```c
typedef struct stab_ctx stab_handle_t;

typedef struct {
    int dx, dy;       // 补偿偏移（像素）
    float angle;      // 旋转角（度）
} stab_transform_t;

typedef void (*stab_data_cb_t)(const stab_transform_t *t, uint64_t timestamp);
```

## 4. 函数API

```c
stab_handle_t *stab_create(const char *iio_dev);
void stab_destroy(stab_handle_t *h);

int stab_start(stab_handle_t *h);
int stab_stop(stab_handle_t *h);

// 获取最新补偿变换
int stab_get_transform(stab_handle_t *h, stab_transform_t *t, uint64_t frame_ts);

// 注册回调
int stab_set_callback(stab_handle_t *h, stab_data_cb_t cb);
```

## 5. 线程安全

- `create/destroy`：非线程安全
- `start/stop`：非线程安全
- `get_transform`：线程安全
- `set_callback`：非线程安全，启动前设置

## 6. 错误码

| 错误码          | 说明             |
| --------------- | ---------------- |
| OCR_ERR_NODEV   | IIO 设备不存在   |
| OCR_ERR_TIMEOUT | 数据读取超时     |

## 7. 示例

```c
stab_handle_t *s = stab_create("/sys/bus/iio/devices/iio:device1");
stab_start(s);

stab_transform_t t;
stab_get_transform(s, &t, frame_timestamp);
// 将 t.dx, t.dy 用于 RGA 裁剪
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
