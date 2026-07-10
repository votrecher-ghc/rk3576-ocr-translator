# IMX415-CSI接口

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 接口规格](#2-接口规格)
- [3. 引脚定义](#3-引脚定义)
- [4. 连接关系](#4-连接关系)
- [5. 电气特性](#5-电气特性)

---

## 1. 概述

IMX415 摄像头通过 MIPI-CSI2 4-lane 接口连接 RK3576，FPC 22pin 排线。

## 2. 接口规格

| 项目       | 规格              |
| ---------- | ----------------- |
| 接口类型   | MIPI-CSI2         |
| Lane 数    | 4                 |
| 数据率     | 最高 1.5Gbps/lane |
| 连接器     | FPC 22pin 0.5mm   |
| 传感器 I2C | I2C4              |

## 3. 引脚定义

| FPC Pin | 信号        | 方向 | RK3576       |
| ------- | ----------- | ---- | ------------ |
| 1       | GND         | -    | GND          |
| 2       | MIPI_D0_N   | I    | CSI0_D0_N    |
| 3       | MIPI_D0_P   | I    | CSI0_D0_P    |
| 4       | GND         | -    | GND          |
| 5       | MIPI_D1_N   | I    | CSI0_D1_N    |
| 6       | MIPI_D1_P   | I    | CSI0_D1_P    |
| 7       | GND         | -    | GND          |
| 8       | MIPI_CLK_N  | I    | CSI0_CLK_N   |
| 9       | MIPI_CLK_P  | I    | CSI0_CLK_P   |
| 10      | GND         | -    | GND          |
| 11      | MIPI_D2_N   | I    | CSI0_D2_N    |
| 12      | MIPI_D2_P   | I    | CSI0_D2_P    |
| 13      | GND         | -    | GND          |
| 14      | MIPI_D3_N   | I    | CSI0_D3_N    |
| 15      | MIPI_D3_P   | I    | CSI0_D3_P    |
| 16      | GND         | -    | GND          |
| 17      | I2C_SDA     | I/O  | I2C4_SDA     |
| 18      | I2C_SCL     | I/O  | I2C4_SCL     |
| 19      | RST_N       | I    | GPIO3_A5     |
| 20      | PWDN        | I    | GPIO3_A4     |
| 21      | VCC_1V8     | -    | 1.8V         |
| 22      | VCC_2V8     | -    | 2.8V（板载） |

## 4. 连接关系

```
IMX415 模组 ──FPC 22pin──► 鲁班猫3 CSI0 接口
                                │
                                ├─ MIPI-CSI2 → DPHY → CISU → ISP
                                ├─ I2C4 → 传感器寄存器配置
                                └─ GPIO3_A4/A5 → 复位/待机控制
```

## 5. 电气特性

- MIPI 差分信号：LVDS 电平
- I2C：1.8V（注意电平）
- 供电：1.8V + 2.8V（模组板载 LDO）

---

> 相关文档：[../03-驱动开发/08-IMX415驱动适配与CSI配置.md](../../03-驱动开发/08-IMX415驱动适配与CSI配置.md)
