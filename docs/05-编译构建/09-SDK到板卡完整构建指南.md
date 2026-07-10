# SDK 到板卡运行的完整构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 基于 LubanCat Linux Generic Full SDK 20260302 |

---

## 目录

- [1. SDK 概述](#1-sdk-概述)
- [2. 环境准备](#2-环境准备)
- [3. SDK 解压与源码检出](#3-sdk-解压与源码检出)
- [4. SDK 目录结构](#4-sdk-目录结构)
- [5. 添加自定义驱动](#5-添加自定义驱动)
- [6. 修改设备树](#6-修改设备树)
- [7. 构建内核](#7-构建内核)
- [8. 交叉编译应用程序](#8-交叉编译应用程序)
- [9. RKNN 模型转换](#9-rknn-模型转换)
- [10. 构建 RootFS](#10-构建-rootfs)
- [11. 打包固件与烧录](#11-打包固件与烧录)
- [12. 板上验证](#12-板上验证)
- [13. 快速参考命令](#13-快速参考命令)

---

## 1. SDK 概述

### 1.1 SDK 版本

- **文件**: `LubanCat_Linux_Generic_Full_SDK_20260302.tgz` (6.7GB)
- **打包日期**: 2026-03-02
- **Manifest**: `lubancat_linux_gen_full.xml`
- **Manifest 分支**: `linux` @ commit `c5ed6b1`
- **远程仓库**: `https://github.com/LubanCat/`

### 1.2 包含的源码仓库 (52 个 project)

| 仓库 | 本地路径 | 分支/标签 | 用途 |
| ---- | -------- | --------- | ---- |
| kernel | `kernel-6.1/` | lbc-develop-6.1 | Linux 6.1 内核源码 |
| kernel | `kernel-5.10/` | lbc-develop-5.10 | Linux 5.10 内核源码 (备用) |
| u-boot | `u-boot/` | main | U-Boot 源码 |
| device_rockchip | `device/rockchip/` | main | 构建脚本/板级配置 |
| buildroot | `buildroot/` | rkr5 | Buildroot 2024.04 |
| rkbin | `rkbin/` | main | Rockchip 预编译二进制 (loader/bl31) |
| tools | `tools/` | main | 烧录/调试工具 |
| gcc-aarch64 | `prebuilts/gcc/.../aarch64/` | - | AArch64 交叉工具链 (gcc 10.3) |
| gcc-arm | `prebuilts/gcc/.../arm/` | - | ARM 交叉工具链 |
| linux-rga | `external/linux-rga/` | rkr5 | **RGA 用户态库源码** |
| rknn-toolkit2 | `external/rknn-toolkit2/` | rkr5 | **RKNN 模型转换工具 (PC端)** |
| rknpu2 | `external/rknpu2/` | rkr5 | **NPU 驱动 + 设备端运行时** |
| mpp | `external/mpp/` | rkr5 | **Rockchip MPP (JPEG硬编码)** |
| camera_engine_rkaiq | `external/camera_engine_rkaiq/` | rkr5 | ISP 摄像头引擎 |
| libmali | `external/libmali/` | - | Mali GPU 库 |
| debian11/12 | `debian11/` `debian12/` | - | 预构建 Debian rootfs |
| ubuntu22.04 | `ubuntu22.04/` | - | 预构建 Ubuntu rootfs |
| lubancat-bin | `lubancat-bin/` | main | 野火板级二进制 (配置/脚本) |

### 1.3 关键发现

SDK 已包含我们需要的所有依赖库源码，无需单独克隆：
- `external/linux-rga/` → 编译后得到 `librga.so`
- `external/rknpu2/runtime/Linux/librknn_api/aarch64/` → 直接含 `librknnrt.so` 和 `rknn_api.h`
- `external/rknn-toolkit2/` → PC 端模型转换
- `external/mpp/` → JPEG 硬件编码
- `device/rockchip/` → 构建系统 (`build.sh` + `Makefile` + `rkflash.sh`)

---

## 2. 环境准备

### 2.1 主机要求

```bash
# Ubuntu 20.04/22.04 x86_64
# 至少 50GB 磁盘空间 (SDK 解压后约 15GB + 构建产物)
# 至少 8GB RAM

# 安装依赖
sudo apt update
sudo apt install -y build-essential git repo bc bison flex \
    libssl-dev libncurses-dev lz4 liblz4-tool device-tree-compiler \
    python3 python3-pip cmake ninja-build cpio rsync wget

# 安装 repo 工具 (如果尚未安装)
mkdir -p ~/.bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/.bin/repo
chmod a+rx ~/.bin/repo
echo 'export PATH=~/.bin:$PATH' >> ~/.bashrc
source ~/.bashrc
```

### 2.2 将 SDK 传输到 Linux 主机

```bash
# 假设 SDK tgz 文件已从 Windows 复制到 Linux 主机
# 例如: ~/sdk/LubanCat_Linux_Generic_Full_SDK_20260302.tgz

mkdir -p ~/sdk
cp /path/to/LubanCat_Linux_Generic_Full_SDK_20260302.tgz ~/sdk/
cd ~/sdk
```

---

## 3. SDK 解压与源码检出

### 3.1 解压

```bash
cd ~/sdk
tar xzf LubanCat_Linux_Generic_Full_SDK_20260302.tgz
# 解压后得到 .repo/ 目录 (约 6GB)
```

### 3.2 检出源码

```bash
# 使用 repo 本地检出 (不需要网络)
.repo/repo/repo sync -l

# 检出完成后，SDK 目录结构如下：
# ~/sdk/
# ├── .repo/
# ├── kernel-6.1/          ← 我们的内核源码
# ├── kernel-5.10/
# ├── u-boot/
# ├── device/rockchip/
# ├── buildroot/
# ├── rkbin/
# ├── tools/
# ├── prebuilts/
# ├── external/
# │   ├── linux-rga/       ← RGA 库源码
# │   ├── rknn-toolkit2/   ← RKNN 工具
# │   ├── rknpu2/          ← NPU 运行时
# │   ├── mpp/             ← MPP 媒体处理
# │   └── ...
# ├── lubancat-bin/
# ├── build.sh             ← ← 链接自 device/rockchip (顶层构建入口)
# ├── Makefile             ← ← 链接自 device/rockchip
# ├── envsetup.sh          ← ← 链接自 buildroot
# └── rkflash.sh           ← ← 链接自 device/rockchip
```

### 3.3 验证

```bash
# 确认内核源码已检出
ls kernel-6.1/Makefile && echo "kernel OK"

# 确认构建脚本存在
ls build.sh Makefile rkflash.sh && echo "build scripts OK"

# 确认工具链
ls prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc && echo "toolchain OK"

# 确认 RGA 和 RKNN
ls external/linux-rga/include/rga.h && echo "RGA OK"
ls external/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so && echo "RKNN runtime OK"
```

---

## 4. SDK 目录结构

```
~/sdk/                           # SDK 根目录
├── build.sh                     # 顶层构建脚本 (链接)
├── Makefile                     # 顶层 Makefile (链接)
├── envsetup.sh                  # Buildroot 环境脚本 (链接)
├── rkflash.sh                   # 烧录脚本 (链接)
│
├── kernel-6.1/                  # ===== Linux 6.1 内核 =====
│   ├── arch/arm64/boot/dts/rockchip/   # DTS 文件目录
│   ├── drivers/                 # 内核驱动
│   ├── Makefile
│   └── ...
│
├── u-boot/                      # ===== U-Boot =====
│
├── device/rockchip/             # ===== 板级配置与构建脚本 =====
│   ├── common/
│   │   ├── scripts/
│   │   │   ├── build.sh         # 核心构建逻辑
│   │   │   └── rkflash.sh       # 烧录逻辑
│   │   └── Makefile
│   └── rk3576/                  # RK3576 板级配置
│       ├── lubancat3/           # 鲁班猫3 配置
│       └── ...
│
├── buildroot/                   # ===== Buildroot 2024.04 =====
│   ├── configs/                 # defconfig 文件
│   ├── package/                 # 软件包
│   └── build/envsetup.sh
│
├── external/                    # ===== 外部库/工具 =====
│   ├── linux-rga/               # RGA 库 (librga.so)
│   ├── rknn-toolkit2/           # RKNN PC 端转换工具
│   ├── rknpu2/                  # NPU 设备端运行时
│   ├── mpp/                     # Rockchip MPP (JPEG 编码)
│   ├── camera_engine_rkaiq/     # ISP 摄像头引擎
│   └── ...
│
├── prebuilts/                   # ===== 预构建工具链 =====
│   └── gcc/linux-x86/aarch64/
│       └── gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/
│           └── bin/aarch64-none-linux-gnu-gcc
│
├── rkbin/                       # ===== Rockchip 预编译二进制 =====
│   └── bin/rk35/rk3576/         # RK3576 loader/bl31/bl32
│
└── tools/                       # ===== 烧录/调试工具 =====
    └── ...
```

---

## 5. 添加自定义驱动

### 5.1 复制驱动源码

将项目中的 5 个自研驱动复制到内核源码目录：

```bash
SDK_ROOT=~/sdk
PROJ_DIR=/path/to/OCR/project   # Windows 侧的项目目录

# 创建驱动目录
mkdir -p ${SDK_ROOT}/kernel-6.1/drivers/ocr

# 复制驱动源码
cp ${PROJ_DIR}/bsp/kernel/drivers/gpio_keys_ocr.c    ${SDK_ROOT}/kernel-6.1/drivers/ocr/
cp ${PROJ_DIR}/bsp/kernel/drivers/iio_ap3216c.c       ${SDK_ROOT}/kernel-6.1/drivers/ocr/
cp ${PROJ_DIR}/bsp/kernel/drivers/iio_icm42688_spi.c  ${SDK_ROOT}/kernel-6.1/drivers/ocr/
cp ${PROJ_DIR}/bsp/kernel/drivers/iio_adt7410.c       ${SDK_ROOT}/kernel-6.1/drivers/ocr/
cp ${PROJ_DIR}/bsp/kernel/drivers/pwm_fan_ocr.c       ${SDK_ROOT}/kernel-6.1/drivers/ocr/
```

### 5.2 添加 Kconfig 和 Makefile

```bash
cp ${PROJ_DIR}/bsp/kernel/drivers/Kconfig  ${SDK_ROOT}/kernel-6.1/drivers/ocr/Kconfig
cp ${PROJ_DIR}/bsp/kernel/drivers/Makefile ${SDK_ROOT}/kernel-6.1/drivers/ocr/Makefile
```

### 5.3 修改内核 drivers/Kconfig

在 `kernel-6.1/drivers/Kconfig` 末尾添加：

```kconfig
source "drivers/ocr/Kconfig"
```

### 5.4 修改内核 drivers/Makefile

在 `kernel-6.1/drivers/Makefile` 末尾添加：

```makefile
obj-$(CONFIG_OCR_DRIVERS) += ocr/
```

### 5.5 启用驱动编译

在内核 defconfig 中添加（或通过 `make menuconfig` 勾选）：

```
CONFIG_OCR_DRIVERS=y
CONFIG_OCR_GPIO_KEYS=y
CONFIG_OCR_AP3216C=y
CONFIG_OCR_ICM42688=y
CONFIG_OCR_ADT7410=y
CONFIG_OCR_PWM_FAN=y
```

---

## 6. 修改设备树

### 6.1 找到板级 DTS

```bash
# 查找鲁班猫3 RK3576 的 DTS 文件
find ${SDK_ROOT}/kernel-6.1/arch/arm64/boot/dts/rockchip/ -name "*lubancat*" -o -name "*rk3576*" | grep -i lubancat
```

预期找到类似 `rk3576-lubancat3.dts` 或 `rk3576-lubancat-3.dts` 文件。

### 6.2 添加外设节点

在板级 DTS 文件中（或通过新建 `.dtsi` 包含文件）添加我们的外设节点：

```dts
/* ===== OCR 翻译系统外设配置 ===== */

/* I2C 总线: AP3216C + ADT7410 */
&i2c4 {   /* 实际总线号需根据原理图确认 */
    status = "okay";

    /* AP3216C 环境光传感器 */
    ap3216c@1e {
        compatible = "ocr,ap3216c";
        reg = <0x1e>;
    };

    /* ADT7410 温度传感器 */
    adt7410@48 {
        compatible = "ocr,adt7410";
        reg = <0x48>;
    };
};

/* SPI 总线: ICM42688 */
&spi0 {   /* 实际总线号需根据原理图确认 */
    status = "okay";

    icm42688@0 {
        compatible = "ocr,icm42688";
        reg = <0>;
        spi-max-frequency = <24000000>;
    };
};

/* PWM 风扇 */
&pwm11 {   /* 实际 PWM 通道需根据原理图确认 */
    status = "okay";

    fan: pwm-fan {
        compatible = "ocr,pwm-fan";
        pwms = <&pwm11 0 50000 0>;
        temp-levels = <40000 55000 70000>;
        pwm-duties = <0 128 255>;
        hysteresis = <3000>;
    };
};

/* 按键 */
/ {
    ocr_keys: gpio_keys_ocr {
        compatible = "ocr,gpio-keys";
        pinctrl-names = "default";

        key-capture {
            label = "key_capture";
            gpios = <&gpio0 16 GPIO_ACTIVE_LOW>;  /* GPIO0_PC0, 需根据原理图确认 */
            linux,code = <212>;  /* KEY_CAMERA = 0xd4 */
            debounce-interval = <20>;
        };
    };
};
```

> **注意**: GPIO 引脚号、I2C/SPI/PWM 总线号需要根据鲁班猫3原理图和 40Pin 引脚表确认。
> IMX415 摄像头和 7 寸屏幕的 DTS 配置保持出厂不动。

### 6.3 编译 DTS Overlay (可选)

如果使用 Overlay 方式：

```bash
cd ${SDK_ROOT}/kernel-6.1
# 编译 overlay
scripts/Makefile.dtc -I dts -O dtb -o overlays/ap3216c.dtbo overlays/ap3216c.dtso
```

---

## 7. 构建内核

### 7.1 选择板级配置

```bash
cd ${SDK_ROOT}

# 列出可用板级配置
./build.sh lunch

# 选择鲁班猫3 RK3576 对应的配置 (具体名称根据列表选择)
# 例如: lubancat3-rk3576-buildroot
```

### 7.2 构建内核

```bash
cd ${SDK_ROOT}

# 方式1: 使用 SDK 顶层 build.sh
./build.sh kernel

# 方式2: 手动构建
cd kernel-6.1
export ARCH=arm64
export CROSS_COMPILE=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

# 加载默认配置
make lubancat_linux_rk3576_defconfig   # 具体名称需确认

# 如需修改配置
make menuconfig
# → Device Drivers → ocr drivers → 勾选所有 OCR 驱动

# 编译
make -j$(nproc) Image dtbs modules
```

### 7.3 产物

```
kernel-6.1/arch/arm64/boot/Image                    # 内核镜像
kernel-6.1/arch/arm64/boot/dts/rockchip/*.dtb       # 设备树
kernel-6.1/drivers/ocr/*.ko                          # 驱动模块 (如果编为模块)
```

---

## 8. 交叉编译应用程序

### 8.1 准备依赖库

```bash
SDK_ROOT=~/sdk

# librga (从 SDK 源码编译)
cd ${SDK_ROOT}/external/linux-rga
mkdir build && cd build
cmake .. -DCMAKE_C_COMPILER=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-gcc
make -j$(nproc)
# 产物: librga.so

# librknnrt (直接使用预编译库)
ls ${SDK_ROOT}/external/rknpu2/runtime/Linux/librknn_api/aarch64/
# librknnrt.so + rknn_api.h

# libdrm (Buildroot 中已有，或从板卡拷贝)
```

### 8.2 编译应用程序

```bash
SDK_ROOT=~/sdk
PROJ_DIR=/path/to/OCR/project
TOOLCHAIN=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

cd ${PROJ_DIR}/app
mkdir build && cd build

cmake .. \
    -DCMAKE_C_COMPILER=${TOOLCHAIN}gcc \
    -DCMAKE_CXX_COMPILER=${TOOLCHAIN}g++ \
    -DCMAKE_FIND_ROOT_PATH=${SDK_ROOT}/external/rknpu2/runtime/Linux/librknn_api/aarch64 \
    -DRGA_LIB_DIR=${SDK_ROOT}/external/linux-rga/build \
    -DRKNN_LIB_DIR=${SDK_ROOT}/external/rknpu2/runtime/Linux/librknn_api/aarch64 \
    -DCMAKE_BUILD_TYPE=Release

make -j$(nproc)

# 产物: ocr_translator 可执行文件
```

### 8.3 也可使用 SDK 工具链环境

```bash
# SDK 提供了环境设置脚本
source ${SDK_ROOT}/buildroot/build/envsetup.sh
# 或
export PATH=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin:$PATH
```

---

## 9. RKNN 模型转换

### 9.1 安装 RKNN Toolkit2 (PC 端)

```bash
cd ${SDK_ROOT}/external/rknn-toolkit2/rknn-toolkit2
pip install -r packages/x86_64/requirements_cp310-*.txt
pip install packages/x86_64/rknn_toolkit2-*-cp310-*.whl
```

### 9.2 转换模型

```bash
PROJ_DIR=/path/to/OCR/project
cd ${PROJ_DIR}/models/convert

# 转换文字检测模型 (INT8)
python convert_ppocr_det.py --target rk3576 --quantize int8

# 转换文字识别模型 (FP16)
python convert_ppocr_rec.py --target rk3576 --quantize fp16

# 转换翻译模型 (FP16)
python convert_transformer.py --target rk3576 --quantize fp16

# 产物:
# ppocrv4_det_int8.rknn
# ppocrv4_rec_fp16.rknn
# lite_transformer_fp16.rknn
```

---

## 10. 构建 RootFS

### 10.1 使用 Buildroot

```bash
cd ${SDK_ROOT}

# 选择 Buildroot rootfs
./build.sh rootfs

# 或手动:
cd buildroot
# 选择鲁班猫3 RK3576 配置
make lubancat3_rk3576_defconfig  # 具体名称需确认

# 添加自定义包 (可选)
# 在 buildroot/package/ 下添加 ocr_app 包

# 构建
make -j$(nproc)

# 产物: output/images/rootfs.ext4
```

### 10.2 将应用和模型注入 RootFS

```bash
PROJ_DIR=/path/to/OCR/project
ROOTFS_DIR=${SDK_ROOT}/buildroot/output/target

# 创建目录
mkdir -p ${ROOTFS_DIR}/usr/share/ocr/models
mkdir -p ${ROOTFS_DIR}/usr/share/ocr/config
mkdir -p ${ROOTFS_DIR}/etc/ocr
mkdir -p ${ROOTFS_DIR}/data/ocr

# 复制应用二进制
cp ${PROJ_DIR}/app/build/ocr_translator ${ROOTFS_DIR}/usr/bin/

# 复制 RKNN 模型
cp ${PROJ_DIR}/models/*.rknn ${ROOTFS_DIR}/usr/share/ocr/models/

# 复制字典
cp ${PROJ_DIR}/models/dict/* ${ROOTFS_DIR}/usr/share/ocr/models/

# 复制配置
cp ${PROJ_DIR}/config/*.json ${ROOTFS_DIR}/etc/ocr/

# 复制运行时库
cp ${SDK_ROOT}/external/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so ${ROOTFS_DIR}/usr/lib/
cp ${SDK_ROOT}/external/linux-rga/build/librga.so ${ROOTFS_DIR}/usr/lib/

# 复制启动脚本
cp ${PROJ_DIR}/bsp/buildroot/board/rockchip/rk3576-lubancat3/rootfs_overlay/etc/init.d/S50ocr \
   ${ROOTFS_DIR}/etc/init.d/
chmod +x ${ROOTFS_DIR}/etc/init.d/S50ocr

# 重新打包 rootfs
cd ${SDK_ROOT}/buildroot
make -j$(nproc)
```

---

## 11. 打包固件与烧录

### 11.1 打包固件

```bash
cd ${SDK_ROOT}

# 使用 SDK 顶层脚本打包
./build.sh firmware

# 或使用打包工具
./build.sh all  # 构建所有组件并打包

# 产物通常在:
# ${SDK_ROOT}/rockdev/
# ├── boot.img          # 含 kernel + dtb
# ├── rootfs.img        # Buildroot rootfs
# ├── uboot.img         # U-Boot
# ├── idbloader.img     # Loader
# └── update.img        # 完整烧录镜像
```

### 11.2 烧录到 eMMC

#### 方式1: 使用 rkflash.sh

```bash
cd ${SDK_ROOT}

# 将板卡进入 Loader 模式 (按住 RECOVERY 键上电)
# 确认设备被识别
lsusb | grep Rockchip

# 烧录
./rkflash.sh
```

#### 方式2: 使用 upgrade_tool

```bash
cd ${SDK_ROOT}/tools

# 烧录完整镜像
./upgrade_tool uf ${SDK_ROOT}/rockdev/update.img

# 或分区烧录
./upgrade_tool ul ${SDK_ROOT}/rockdev/uboot.img       # Loader
./upgrade_tool di -k ${SDK_ROOT}/rockdev/boot.img      # Kernel
./upgrade_tool di -r ${SDK_ROOT}/rockdev/rootfs.img    # RootFS
```

#### 方式3: SD 卡启动 (调试用)

```bash
# 使用 SDK 提供的 SD 卡制作脚本
./mkfirmware.sh sdupdate
# 或手动写入 SD 卡
dd if=${SDK_ROOT}/rockdev/update.img of=/dev/sdX bs=4M
```

---

## 12. 板上验证

### 12.1 启动验证

```bash
# 串口连接 (1500000 baud)
minicom -D /dev/ttyUSB0 -b 1500000

# 上电后应看到 U-Boot → Kernel → 登录提示符
# 登录后检查内核版本
uname -r   # 应为 6.1.x
```

### 12.2 逐项验证

```bash
# 1. 屏幕显示 (出厂已验证)
cat /sys/class/backlight/panel/brightness

# 2. 摄像头 (出厂已验证)
v4l2-ctl --device=/dev/video0 --list-formats-ext
v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1

# 3. 按键 (自研驱动)
evtest /dev/input/event0    # 按下按键应看到 KEY_CAMERA 事件

# 4. AP3216C 环境光 (自研驱动)
cat /sys/bus/iio/devices/iio:device0/in_illuminance_raw

# 5. ICM42688 IMU (自研驱动)
cat /sys/bus/iio/devices/iio:device1/in_accel_x_raw

# 6. ADT7410 温度 (自研驱动)
cat /sys/bus/iio/devices/iio:device2/in_temp_input

# 7. PWM 风扇 (自研驱动)
cat /sys/class/hwmon/hwmon0/fan1_target

# 8. RKNN 运行时
ls /usr/lib/librknnrt.so

# 9. 应用启动
/usr/bin/ocr_translator --config /etc/ocr/ocr_translator.json
```

### 12.3 验证顺序

```
屏幕显示 → 摄像头出图 → 按键事件 → 环境光数值 → IMU数据
→ 温度数值 → 风扇转动 → 零拷贝管线 → OCR推理 → 翻译 → 全功能
```

---

## 13. 快速参考命令

```bash
# ===== 一键构建 =====
cd ~/sdk
./build.sh lunch        # 选择板级配置
./build.sh all          # 构建全部
./rkflash.sh            # 烧录

# ===== 单独构建 =====
./build.sh kernel       # 仅内核
./build.sh uboot        # 仅 U-Boot
./build.sh rootfs       # 仅 RootFS

# ===== 内核手动构建 =====
cd kernel-6.1
export ARCH=arm64
export CROSS_COMPILE=$(pwd)/../prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
make lubancat_linux_rk3576_defconfig
make menuconfig         # 添加 OCR 驱动
make -j$(nproc) Image dtbs modules

# ===== 应用编译 =====
cd /path/to/project/app
mkdir build && cd build
cmake .. -DCMAKE_C_COMPILER=aarch64-none-linux-gnu-gcc -DCMAKE_CXX_COMPILER=aarch64-none-linux-gnu-g++
make -j$(nproc)

# ===== 模型转换 =====
cd /path/to/project/models/convert
python convert_ppocr_det.py --target rk3576 --quantize int8
python convert_ppocr_rec.py --target rk3576 --quantize fp16
python convert_transformer.py --target rk3576 --quantize fp16
```

---

> **注意**: 本文档中的 defconfig 名称、DTS 文件名、GPIO 引脚号等需要根据实际 SDK 检出后的文件确认。
> 在 SDK `repo sync -l` 完成后，运行 `find kernel-6.1/arch/arm64/boot/dts/rockchip/ -name "*lubancat*"` 确认 DTS 文件名，
> 运行 `ls device/rockchip/rk3576/` 确认板级配置名称。
