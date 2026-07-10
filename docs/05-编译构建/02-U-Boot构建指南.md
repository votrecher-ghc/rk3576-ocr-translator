# U-Boot构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 配置](#2-配置)
- [3. 编译](#3-编译)
- [4. 环境变量](#4-环境变量)
- [5. 烧录](#5-烧录)

---

## 1. 概述

U-Boot 2017.09（瑞芯微定制版），负责 RK3576 板级初始化与内核引导。

## 2. 配置

```bash
cd u-boot
# 使用鲁班猫3 默认配置
make CROSS_COMPILE=aarch64-linux-gnu- rk3576_lbc3_defconfig
# 自定义配置
make CROSS_COMPILE=aarch64-linux-gnu- menuconfig
```

## 3. 编译

```bash
make CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc)
# 产物：u-boot.bin, idbloader.img, u-boot.itb
```

## 4. 环境变量

关键启动参数（`include/configs/rk3576_lbc3.h`）：
- bootcmd：从 eMMC boot 分区加载内核
- bootargs：console=ttyS2 root=/dev/mmcblk0p3 cma=256M

## 5. 烧录

```bash
# 使用 upgrade_tool 烧录
upgrade_tool ul idbloader.img        # loader
upgrade_tool ub u-boot.itb           # u-boot
```

---

> 相关文档：[03-Kernel构建指南.md](03-Kernel构建指南.md)
