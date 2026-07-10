# TFTP-NFS调试环境搭建

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. TFTP 服务器搭建](#2-tftp-服务器搭建)
- [3. NFS 服务器搭建](#3-nfs-服务器搭建)
- [4. U-Boot 配置](#4-u-boot-配置)
- [5. 使用方法](#5-使用方法)

---

## 1. 概述

通过 TFTP 加载内核、NFS 挂载根文件系统，加速开发调试，无需每次烧录 eMMC。

## 2. TFTP 服务器搭建

```bash
# 主机安装
sudo apt install tftpd-hpa
sudo mkdir /tftpboot && chmod 777 /tftpboot

# 配置 /etc/default/tftpd-hpa
TFTP_DIRECTORY="/tftpboot"
TFTP_OPTIONS="--secure"

sudo systemctl restart tftpd-hpa

# 放置内核与 dtb
cp arch/arm64/boot/Image /tftpboot/
cp arch/arm64/boot/dts/rockchip/rk3576-lbc3.dtb /tftpboot/
```

## 3. NFS 服务器搭建

```bash
sudo apt install nfs-kernel-server
sudo mkdir /nfsroot

# /etc/exports
/nfsroot *(rw,sync,no_root_squash,no_subtree_check)

sudo systemctl restart nfs-kernel-server

# 解压根文件系统
sudo tar xf rootfs.tar -C /nfsroot
```

## 4. U-Boot 配置

```
setenv serverip 192.168.1.100
setenv ipaddr 192.168.1.50
setenv bootcmd 'tftp 0x40600000 Image; tftp 0x44000000 rk3576-lbc3.dtb; booti 0x40600000 - 0x44000000'
setenv bootargs 'console=ttyS2,1500000 root=/dev/nfs nfsroot=192.168.1.100:/nfsroot ip=192.168.1.50:192.168.1.100:192.168.1.1:255.255.255.0::eth0:off rw'
saveenv
```

## 5. 使用方法

- 修改内核后：`cp Image /tftpboot/` → 重启板子
- 修改应用后：直接在 `/nfsroot/ocr/bin/` 替换，板子重启
- 无需重新烧录 eMMC

---

> 相关文档：[06-硬件接口参考/04-调试接口.md](../06-硬件接口参考/04-调试接口.md)
