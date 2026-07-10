# Kernel构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 内核配置](#2-内核配置)
- [3. 编译](#3-编译)
- [4. 设备树编译](#4-设备树编译)
- [5. 模块编译](#5-模块编译)

---

## 1. 概述

Linux 6.1（分支 lbc-develop-6.1），包含 RK3576 驱动支持。

## 2. 内核配置

```bash
cd kernel
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 lbc3_defconfig
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 menuconfig
```

关键配置项：
```
CONFIG_CMA=y
CONFIG_CMA_SIZE_MBYTES=256
CONFIG_DRM_ROCKCHIP=y
CONFIG_VIDEO_ROCKCHIP_ISP=y
CONFIG_IIO=y
CONFIG_PWM=y
CONFIG_RKNPU=y
```

## 3. 编译

```bash
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 -j$(nproc)
# 产物：arch/arm64/boot/Image
```

## 4. 设备树编译

```bash
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 dtbs
# 产物：arch/arm64/boot/dts/rockchip/rk3576-lbc3.dtb

# Overlay 编译
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 rk3576-lbc3-ap3216c.dtbo
```

## 5. 模块编译

```bash
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 modules -j$(nproc)
make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 modules_install INSTALL_MOD_PATH=/path/to/rootfs
```

---

> 相关文档：[04-Buildroot构建指南.md](04-Buildroot构建指南.md)
