# IIO用户空间接口

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. sysfs 接口](#2-sysfs-接口)
- [3. buffer 接口](#3-buffer-接口)
- [4. 常用通道名](#4-常用通道名)

---

## 1. 概述

IIO（Industrial I/O）子系统提供 sysfs（单次读取）和 buffer（连续流）两种用户空间接口。

## 2. sysfs 接口

```bash
# 设备列表
ls /sys/bus/iio/devices/

# 设备名称
cat /sys/bus/iio/devices/iio:device0/name

# 单次读取
cat /sys/bus/iio/devices/iio:device0/in_temp_input        # 温度（毫摄氏度）
cat /sys/bus/iio/devices/iio:device0/in_illuminance_raw   # 环境光
cat /sys/bus/iio/devices/iio:device0/in_anglvel_x_raw     # 陀螺X
cat /sys/bus/iio/devices/iio:device0/in_accel_x_raw       # 加速度X

# 采样率
cat /sys/bus/iio/devices/iio:device0/in_anglvel_sampling_frequency
echo 1000 > /sys/bus/iio/devices/iio:device0/in_anglvel_sampling_frequency
```

## 3. buffer 接口

```bash
# 启用通道
echo 1 > /sys/bus/iio/devices/iio:device1/scan_elements/in_anglvel_x_en
echo 1 > /sys/bus/iio/devices/iio:device1/scan_elements/in_accel_x_en

# 设置水印
echo 100 > /sys/bus/iio/devices/iio:device1/buffer/watermark

# 启动
echo 1 > /sys/bus/iio/devices/iio:device1/buffer/enable

# 读取（二进制）
cat /dev/iio:device1 > data.bin
```

## 4. 常用通道名

| 传感器类型 | 通道名               | 单位           |
| ---------- | -------------------- | -------------- |
| 温度       | in_temp_input        | 毫摄氏度       |
| 环境光     | in_illuminance_raw   | lux            |
| 陀螺仪     | in_anglvel_x/y/z_raw | rad/s          |
| 加速度计   | in_accel_x/y/z_raw   | m/s²           |
| 接近       | in_proximity_raw     | -              |

---

> 相关文档：[03-驱动开发/01-内核子系统概览.md](../03-驱动开发/01-内核子系统概览.md)
