# BSP与系统构建

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. BSP 适配范围](#2-bsp-适配范围)
- [3. U-Boot 适配](#3-u-boot-适配)
- [4. 内核适配](#4-内核适配)
- [5. Buildroot 定制](#5-buildroot-定制)
- [6. 设备树组织](#6-设备树组织)
- [7. 关键配置项](#7-关键配置项)

---

## 1. 概述

BSP（Board Support Package）层负责 U-Boot、Linux 内核、根文件系统的适配与构建，为上层应用提供可运行的系统底座。

## 2. BSP 适配范围

| 组件       | 版本          | 适配内容                           |
| ---------- | ------------- | ---------------------------------- |
| U-Boot     | 2017.09       | 板级初始化、启动参数、分区表       |
| Linux      | 6.1 (lbc-dev) | 驱动使能、设备树、内核配置         |
| Buildroot  | 2023.02       | 包选择、根文件系统定制、脚本       |

## 3. U-Boot 适配

- 板级文件：`board/rockchip/rk3576_lbc3/`
- 默认环境变量配置（bootcmd/bootargs）
- 分区表定义（GPT）
- 启动参数：console、root、CMA 大小

## 4. 内核适配

- defconfig：`arch/arm64/configs/lbc3_defconfig`
- 关键配置：CMA、DRM、V4L2、IIO、PWM、RGA、RKNN
- 设备树：`arch/arm64/boot/dts/rockchip/rk3576-lbc3.dts`

## 5. Buildroot 定制

- 自定义 defconfig
- 预装包：libdrm、librga、rknn_api、openssl、json-c
- 启动脚本：S50ocr_translate
- 固件打包：mkrootfs + 固件分区镜像

## 6. 设备树组织

```
rk3576-lbc3.dts          // 板级入口
├── rk3576.dtsi          // SoC 基础
├── rk3576-pinctrl.dtsi  // 引脚复用
└── overlays/            // 外设 Overlay
    ├── imx415.dtso
    ├── ap3216c.dtso
    └── ...
```

## 7. 关键配置项

- CMA: 256MB
- V4L2: imx415 使能
- DRM: rockchip-drm 使能
- IIO: ap3216c/icm42688/adt7410 使能
- PWM: fan 使能

> 详见 [../05-编译构建/](../05-编译构建/)

---

> 相关文档：[02-驱动与外设适配.md](02-驱动与外设适配.md)
