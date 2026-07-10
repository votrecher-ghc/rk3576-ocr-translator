# Buildroot构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 配置](#2-配置)
- [3. 自定义包](#3-自定义包)
- [4. 编译](#4-编译)
- [5. 根文件系统](#5-根文件系统)

---

## 1. 概述

Buildroot 2023.02，自动化生成交叉工具链、内核、根文件系统。

## 2. 配置

```bash
cd buildroot
make rockchip_rk3576_lbc3_defconfig
make menuconfig
```

关键配置：
- Target arch: arm64
- Toolchain: external (aarch64-linux-gnu)
- System: /bin/busybox init
- Packages: libdrm, librga, json-c, openssl

## 3. 自定义包

```
package/ocr_translate/
├── Config.in
├── ocr_translate.mk
└── src/  (或指向外部源码)
```

## 4. 编译

```bash
make -j$(nproc)
# 产物：output/images/rootfs.ext4
```

## 5. 根文件系统

```bash
# 打包
mkdir rootfs && cd rootfs
tar xf ../output/images/rootfs.tar
# 添加自定义文件
cp /path/to/ocr_translate usr/bin/
```

---

> 相关文档：[05-应用CMake构建指南.md](05-应用CMake构建指南.md)
