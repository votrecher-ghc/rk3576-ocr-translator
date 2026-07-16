# IIO用户空间接口

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v1.1 | 2026-07-13 | 项目组 | 改为按 name 发现并对齐本项目 channel/scale/buffer ABI |
| v2.0 | 2026-07-16 | 项目组 | 同步最新交接和板端使用约束 |

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

# 按 name 查找，不能把 device 编号写死
find_iio() {
    want="$1"
    for d in /sys/bus/iio/devices/iio:device*; do
        [ "$(cat "$d/name" 2>/dev/null)" = "$want" ] && printf '%s\n' "$d"
    done
}
TEMP="$(find_iio adt7410)"
LIGHT="$(find_iio ap3216c)"
IMU="$(find_iio icm42688)"
[ -n "$TEMP" ] && [ -n "$LIGHT" ] && [ -n "$IMU" ] || exit 1

# 单次读取
cat "$TEMP/in_temp_input"                 # 毫摄氏度
cat "$LIGHT/in_illuminance_raw"
cat "$LIGHT/in_illuminance_scale"         # lux/count
cat "$IMU/in_anglvel_x_raw"
cat "$IMU/in_anglvel_scale"               # rad/s per count
cat "$IMU/in_accel_x_raw"
cat "$IMU/in_accel_scale"                 # m/s² per count
```

当前 ICM42688 驱动固定配置为 1kHz ODR，不暴露可写的
`in_anglvel_sampling_frequency`。不要对不存在的 sysfs 属性写值。

## 3. buffer 接口

```bash
# 先停止 ocr_translator；应用运行时由它独占 buffer
IMU="$(find_iio icm42688)"
[ -n "$IMU" ] || exit 1

# 当前驱动只接受固定 scan mask：温度 + 三轴加速度 + 三轴陀螺仪
for f in "$IMU"/scan_elements/*_en; do echo 1 > "$f"; done

# 设置水印
echo 100 > "$IMU/buffer/watermark"

# 启动
echo 1 > "$IMU/buffer/enable"

# 读取（二进制）
cat "/dev/${IMU##*/}" > data.bin
```

解析二进制前必须读取 `scan_elements/*_{index,type}` 并按 `scan_bytes` 对齐；不要把
结构体布局写死。当前组合通常为 24-byte frame，但仍以目标内核导出的布局为准。

## 4. 常用通道名

| 传感器类型 | 通道名               | 单位           |
| ---------- | -------------------- | -------------- |
| 温度       | `in_temp_input` | 毫摄氏度 |
| 温度       | `in_temp_raw` + `in_temp_offset` + `in_temp_scale` | `(raw + offset) * scale` 为毫摄氏度 |
| 环境光     | `in_illuminance_raw` + `in_illuminance_scale` | `raw * scale` 为 lux |
| 陀螺仪     | `in_anglvel_x/y/z_raw` + `in_anglvel_scale` | `raw * scale` 为 rad/s |
| 加速度计   | `in_accel_x/y/z_raw` + `in_accel_scale` | `raw * scale` 为 m/s² |
| 接近       | `in_proximity_raw` | 10-bit 相对计数 |

---

> 相关文档：[03-驱动开发/01-内核子系统概览.md](../03-驱动开发/01-内核子系统概览.md)
