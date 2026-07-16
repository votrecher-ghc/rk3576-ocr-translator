# SDK 到板卡运行的完整构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 基于 LubanCat Linux Generic Full SDK 20260302 |
| v2.0 | 2026-07-16 | 项目组 | 对齐统一构建脚本、vendor Buildroot external tree、模型校验及安全固件打包流程 |

> 本文以项目中的 `scripts/build/*.sh` 为唯一推荐构建入口。文中的 SDK 路径和
> 板级名称仍需按实际野火/Rockchip SDK 核对；当前工作区未完成目标板交叉编译与
> 实板验收，不能用本文的主机侧检查替代板端验证。

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
| mpp | `external/mpp/` | rkr5 | Rockchip MPP；当前拍照归档仍使用 libjpeg CPU 编码，MPP 接入待板端完成 |
| camera_engine_rkaiq | `external/camera_engine_rkaiq/` | rkr5 | ISP 摄像头引擎 |
| libmali | `external/libmali/` | - | Mali GPU 库 |
| debian11/12 | `debian11/` `debian12/` | - | 预构建 Debian rootfs |
| ubuntu22.04 | `ubuntu22.04/` | - | 预构建 Ubuntu rootfs |
| lubancat-bin | `lubancat-bin/` | main | 野火板级二进制 (配置/脚本) |

### 1.3 关键发现

SDK 已包含目标工具链、RGA/RKNN 运行时和板级构建系统，无需从宿主系统拼装
目标库；真实 ONNX/RKNN 模型、词表、量化图片和 CJK 字体仍需单独准备：
- `external/linux-rga/` → 编译后得到 `librga.so`
- `external/rknpu2/` → 提供 AArch64 `librknnrt.so` 与 `rknn_api.h`，具体子目录随 SDK 版本变化
- `external/rknn-toolkit2/` → PC 端模型转换
- `external/mpp/` → 可供后续接入 MPP JPEG；当前应用构建不依赖它
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

# 确认 RGA 和 RKNN；不同 SDK 小版本的具体目录可能不同
find external/linux-rga -name im2d.h -o -name librga.so
find external/rknpu2 -name rknn_api.h -o -name librknnrt.so
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
obj-y += ocr/
```

项目 Kconfig 没有 `CONFIG_OCR_DRIVERS` 总开关；进入 `ocr/` 目录后，由下面各个真实
符号决定对应对象是否构建。项目的 `scripts/build/build_kernel.sh` 采用 `M=...` 外部
模块方式时，不需要修改上级 `drivers/Makefile`。

### 5.5 启用驱动编译

在内核 defconfig 中添加（或通过 `make menuconfig` 勾选）：

```
CONFIG_OCR_GPIO_KEYS=m
CONFIG_IIO_AP3216C=m
CONFIG_IIO_ICM42688_SPI=m
CONFIG_IIO_ADT7410=m
CONFIG_PWM_FAN_OCR=m
```

`PWM_FAN_OCR` 只是可选的内核态替代实现。默认产品路径由应用独占 PWM sysfs；即使
构建了该模块，也不要在同一 PWM 通道创建 `compatible = "ocr,pwm-fan"` 的 consumer。

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
        spi-cpha;
        spi-cpol;
        interrupt-parent = <&gpio0>;
        interrupts = <10 IRQ_TYPE_EDGE_FALLING>; /* 示例 GPIO0_PB2，必须核对 */
    };
};

/* PWM 风扇：只启用控制器并加用户态发现标记，不创建内核 consumer */
&pwm11 {   /* 实际 PWM 通道需根据原理图确认 */
    status = "okay";
    ocr,fan-pwm;
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
> `imx415-csi2.dtso` 未按 vendor media graph 核对，项目构建默认跳过；只有完成链路、
> 时钟、供电和 endpoint 核对后才可设置 `BUILD_UNVERIFIED_CAMERA_OVERLAY=1` 单独测试。

### 6.3 编译 DTS Overlay (可选)

项目构建会先用内核头文件预处理 overlay，再生成带 `ocr-` 前缀的 DTBO，避免覆盖
vendor 同名产物：

```bash
cd ${PROJ_DIR}
export KERNEL_SRC=${SDK_ROOT}/kernel-6.1
export CROSS_COMPILE=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-
bash scripts/build/build_kernel.sh
# 产物位于 output/kernel/dtbo/ocr-*.dtbo
```

---

## 7. 构建内核

### 7.1 选择板级配置

项目脚本当前使用 `lubancat3_rk3576_defconfig`，目标 DTB 为
`rk3576-lubancat3.dtb`。首次构建前先在实际 SDK 中确认两者存在：

```bash
test -f "${SDK_ROOT}/kernel-6.1/arch/arm64/configs/lubancat3_rk3576_defconfig"
test -f "${SDK_ROOT}/kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576-lubancat3.dts"
```

若 vendor SDK 使用不同名称，应先同步调整 `scripts/build/build_kernel.sh`，不要让
构建成功后再从其他板型产物中猜测 DTB。

### 7.2 使用项目脚本构建

```bash
SDK_ROOT=/path/to/vendor-sdk
PROJ_DIR=/path/to/OCR/project

export KERNEL_SRC="${SDK_ROOT}/kernel-6.1"
export ARCH=arm64
export CROSS_COMPILE="${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"

cd "${PROJ_DIR}"
bash scripts/build/build_kernel.sh
```

该脚本会依次验证 vendor defconfig 的 IIO/PWM/Input 等依赖，构建 Image、base DTB、
vendor 模块、自研 OCR 外部模块和项目 overlay。自研模块在 `modules` 与
`modules_install` 两个阶段使用同一组 Kconfig 参数，最后检查五个 `.ko` 均已进入
模块包。未核对 vendor media graph 的 IMX415 overlay 默认跳过。

### 7.3 统一产物

```
output/kernel/Image
output/kernel/rk3576-lubancat3.dtb
output/kernel/dtbo/ocr-*.dtbo
output/kernel/modules.tar.gz
```

后续 `build_rootfs.sh` 固定读取 `output/kernel/modules.tar.gz`，
`make_image.sh` 固定读取 Image、base DTB 和 DTBO 目录，因此不要绕过该收集步骤。

---

## 8. 交叉编译应用程序

### 8.1 依赖与 SYSROOT 边界

应用必须使用与最终 rootfs 同源的 AArch64 sysroot。`build_app.sh` 会检查编译器
`-dumpmachine`、拒绝把 `/` 当作 sysroot，并要求 `${SYSROOT}/usr/include` 存在。
工具链文件还会：

- 将库、头文件和 CMake package 搜索限制在 `SYSROOT`；
- 清空宿主 `PKG_CONFIG_PATH`，重新设置目标 `PKG_CONFIG_SYSROOT_DIR` 与
  `PKG_CONFIG_LIBDIR`；
- 禁止从宿主 `/usr/include`、`/usr/lib` 或 x86_64 pkg-config 结果静默链接依赖。

`libdrm`、libjpeg、FreeType、cJSON 应由 vendor sysroot 提供。RGA/RKNN 也应优先
安装到同一 sysroot；如果 SDK 将它们放在独立目录，必须显式提供目标架构头文件和
共享库路径。

### 8.2 设置环境并构建

```bash
SDK_ROOT=/path/to/vendor-sdk
PROJ_DIR=/path/to/OCR/project
TOOLCHAIN=${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-

export CROSS_COMPILE="${TOOLCHAIN}"
# 可使用 vendor Buildroot output/staging，或其真实 sysroot 目录。
export SYSROOT=/path/to/vendor-buildroot-output/staging

# 仅当 RGA/RKNN 没有安装进上述 sysroot 时，显式指定目标架构头文件和库；
# 四个路径不得指向宿主 x86_64 产物。
export OCR_RGA_INCLUDE_DIR=/path/to/directory-containing-im2d.h
export OCR_RGA_LIBRARY=/path/to/aarch64/librga.so
export OCR_RKNN_INCLUDE_DIR=/path/to/directory-containing-rknn_api.h
export OCR_RKNN_LIBRARY=/path/to/aarch64/librknnrt.so

cd "${PROJ_DIR}"
bash scripts/build/build_app.sh

# 产物: output/app/ocr_translator
```

若 RGA/RKNN 已正确安装到 `SYSROOT`，应取消对应显式变量，让 CMake 从 sysroot
发现它们：

```bash
unset OCR_RGA_INCLUDE_DIR OCR_RGA_LIBRARY
unset OCR_RKNN_INCLUDE_DIR OCR_RKNN_LIBRARY
```

仅在使用外置共享库时，设置第 8.2 节的四个变量后再确认其架构：

```bash
"${CROSS_COMPILE}readelf" -h "${OCR_RGA_LIBRARY}" | grep AArch64
"${CROSS_COMPILE}readelf" -h "${OCR_RKNN_LIBRARY}" | grep AArch64
```

`build_app.sh` 每次会清理 `app/build`，以 Release、`BUILD_TESTING=OFF` 重新配置，
并只把最终 AArch64 可执行文件收集到 `output/app/`。不要在文档或自动化中另建一套
手写 CMake 参数；依赖边界应统一由该脚本和
`app/cmake/aarch64-linux.cmake` 维护。

---

## 9. RKNN 模型转换

### 9.1 安装 RKNN Toolkit2 (PC 端)

```bash
cd ${SDK_ROOT}/external/rknn-toolkit2/rknn-toolkit2
pip install -r packages/x86_64/requirements_cp310-*.txt
pip install packages/x86_64/rknn_toolkit2-*-cp310-*.whl
python3 -c "import rknn; print('rknn-toolkit2 OK')"
```

转换主机还需要 `sha256sum`。模型转换是 PC 端任务，不使用板端
`librknnrt.so`；Toolkit2 版本仍必须与目标板驱动/运行时版本匹配。

### 9.2 转换模型

```bash
PROJ_DIR=/path/to/OCR/project
cd "${PROJ_DIR}"

# 以下路径必须指向真实、版本固定且与运行时契约匹配的本地资产。
export DET_ONNX_PATH=/path/to/det.onnx
export DET_QUANT_IMG_DIR=/path/to/calibration_images
export REC_ONNX_PATH=/path/to/rec.onnx
export OCR_REC_VOCAB_PATH=/path/to/ppocr_keys_v1.txt
export TRANS_ENCODER_ONNX_PATH=/path/to/encoder.onnx
export TRANS_DECODER_ONNX_PATH=/path/to/decoder.onnx
export TRANS_SRC_VOCAB_PATH=/path/to/source_vocab.txt
export TRANS_TGT_VOCAB_PATH=/path/to/target_vocab.txt
export TRANS_TOKENIZER_CONTRACT=greedy-vocab-v1
export TRANS_SRC_LANG=en
export TRANS_TGT_LANG=zh

# 可选：覆盖默认 output/models。
# export MODEL_OUTPUT_DIR=/absolute/path/to/model-output

bash scripts/build/build_models.sh

# 立即验证本次产物；build_rootfs.sh 和 post_build.sh 还会再次校验。
(cd "${MODEL_OUTPUT_DIR:-output/models}" && \
  sha256sum -c model_artifacts.sha256)
```

脚本会先删除它所管理的旧模型产物，避免上次构建残留被误部署，然后生成：

```
output/models/ppocrv4_det_int8.rknn
output/models/ppocrv4_rec_fp16.rknn
output/models/ppocr_keys_v1.txt
output/models/lite_transformer_encoder_fp16.rknn
output/models/lite_transformer_decoder_fp16.rknn
output/models/translation_src_vocab.txt
output/models/translation_tgt_vocab.txt
output/models/translation_manifest.json
output/models/model_artifacts.sha256
```

`model_artifacts.sha256` 固定前八个部署资产；任何模型、词表或 manifest 改动都必须
重新运行统一脚本，不能手工修改文件后沿用旧校验清单。

PaddleOCR 官方发布的是 Paddle inference 模型时，应先按 PaddleOCR 的
Paddle2ONNX 流程导出 ONNX，再运行上述 RKNN 转换脚本；不要将 `.tar` 模型包
直接作为 ONNX 输入。翻译转换脚本会拒绝动态序列维度或不匹配的 encoder / decoder
接口，避免生成板端无法加载的模型。
当前板端 tokenizer 是确定性的 UTF-8 词表最长匹配，并不是通用 BPE 或
SentencePiece 实现；只有按 `greedy-vocab-v1` 契约训练/包装并导出的固定语言对
模型可直接部署。普通 Hugging Face tokenizer 还必须在模型外补齐 merges、
normalizer 等算法后才能使用。

---

## 10. 构建 RootFS

### 10.1 输入产物与 vendor Buildroot 要求

`build_rootfs.sh` 只接受野火/Rockchip vendor Buildroot。该树必须识别项目 defconfig
使用的 Rockchip 包符号，尤其是 `BR2_PACKAGE_LIBRGA` 与
`BR2_PACKAGE_RKNN_RUNTIME`；不能用不含这些包的上游 Buildroot 替代。

运行前必须已经存在：

```
output/app/ocr_translator
output/models/model_artifacts.sha256
output/kernel/modules.tar.gz
```

脚本会确认应用是 AArch64 ELF、模型校验清单完整且 `sha256sum -c` 通过、模块压缩包
可读取。缺少任一输入都会在启动 Buildroot 前失败。
仅在产物确实位于其他目录时，才通过 `APP_BIN`、`MODEL_DIR` 或
`MODULE_ARCHIVE` 覆盖这些默认路径。

### 10.2 使用 BR2_EXTERNAL 构建

项目的 Buildroot external tree 位于 `bsp/buildroot/`，包含 `external.desc`、
`external.mk`、`Config.in`、项目 defconfig、rootfs overlay 和 post-build 脚本。
统一入口会自动以
`BR2_EXTERNAL="${PROJ_DIR}/bsp/buildroot"` 调用 vendor Buildroot，不需要把这些
文件复制进 SDK：

```bash
SDK_ROOT=/path/to/vendor-sdk
PROJ_DIR=/path/to/OCR/project

export BUILDROOT_SRC="${SDK_ROOT}/buildroot"
# 可使用宿主 readelf，也可显式使用目标工具链的 readelf。
export READELF="${CROSS_COMPILE}readelf"

# 若 vendor rootfs 未安装目标字体，显式提供可分发的 Noto CJK 文件。
export FONT_FILE=/path/to/NotoSansCJK-Regular.ttc

cd "${PROJ_DIR}"
bash scripts/build/build_rootfs.sh
```

脚本使用独立输出树 `output/buildroot-build/`，加载
`ocr_translator_defconfig` 后会复核架构、工具链、SysV init、udev、ext4，以及
libdrm/RGA/RKNN/FreeType/cJSON/jpeg/libiio 等必需符号，防止 vendor Buildroot
静默忽略不认识的配置。

正常情况下，`librga.so` 与 `librknnrt.so` 应由 vendor Buildroot 包安装到 target
rootfs。只有包已被 vendor tree 正确识别、但产物位于独立 AArch64 目录时，才使用
post-build 的显式 fallback：

```bash
export RGA_LIB_DIR=/path/to/aarch64/rga-libraries
export RKNN_LIB_DIR=/path/to/aarch64/rknn-libraries
bash scripts/build/build_rootfs.sh
```

fallback 目录中的库仍会经过 AArch64 ELF 校验，并连同版本化符号链接一起复制；
它们不能用于绕过缺少 Rockchip 包定义的上游 Buildroot。

### 10.3 注入内容与产物

post-build 会自动完成以下工作：

- 安装 vendor 与 OCR 内核模块；
- 再次校验并复制模型、词表、翻译 manifest 和 `model_artifacts.sha256`；
- 只部署 `config/ocr_translator.json`，不部署设计参考用的
  `pipeline.json`/`sensors.json`；
- 安装并校验 AArch64 应用、RGA/RKNN 运行时及 Noto CJK 字体；
- 保留 rootfs overlay 中的 `S50ocr`，创建 `/data/ocr`。

最终文件路径为：

```
output/buildroot-build/images/rootfs.ext4   # Buildroot 原始产物
output/rootfs/rootfs.ext4                    # 项目统一收集产物
```

后续固件脚本固定读取 `output/rootfs/rootfs.ext4`，不要再手工向 SDK 的
`output/target` 复制文件或另行重新打包。

---

## 11. 打包固件与烧录

### 11.1 打包固件

`make_image.sh` 固定读取以下统一产物：

```
output/uboot/idbloader.img
output/uboot/u-boot.itb
output/kernel/Image
output/kernel/rk3576-lubancat3.dtb
output/kernel/dtbo/
output/rootfs/rootfs.ext4
```

未设置 `OCR_DTBO_LIST` 时，安全默认是使用未经修改的 vendor base DTB，不自动选择
目录中的任何 overlay：

```bash
PROJ_DIR=/path/to/OCR/project
cd "${PROJ_DIR}"

unset OCR_DTBO_LIST
unset DT_OVERLAY_MODE
unset RK_PACK_SCRIPT
bash scripts/build/make_image.sh
```

此时会生成分区镜像集合，但不会伪造 `update.img`。只有逐项核对原理图、base DTS、
GPIO/总线/供电名称后，才可从 `output/kernel/dtbo/` 显式选择 overlay。

#### merged 模式

```bash
export OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-adt7410.dtbo ocr-pwm-fan.dtbo"
export DT_OVERLAY_MODE=merged
unset RK_PACK_SCRIPT
bash scripts/build/make_image.sh
```

脚本通过 `fdtoverlay` 生成
`output/firmware/rk3576-lubancat3-ocr.dtb`，并只把合并后的 DTB 写进
`boot.img`；不会再把同一 DTBO 复制到 vendor resource。若还需生成完整
`update.img`，可在 merged 模式下另行设置同一 vendor `RK_PACK_SCRIPT`。

#### vendor-resource 模式

```bash
export OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-adt7410.dtbo ocr-pwm-fan.dtbo"
export DT_OVERLAY_MODE=vendor-resource
export RK_PACK_SCRIPT=/absolute/path/to/vendor-pack-wrapper
bash scripts/build/make_image.sh
```

该模式保持 FIT 内的 base DTB 不变，把选择的 DTBO 暂存到
`output/firmware/dtbo/`，并强制要求可执行的 vendor 包装器实际启用它们。
`RK_PACK_SCRIPT` 的接口必须是：

```text
vendor-pack-wrapper <output/firmware目录> <目标update.img路径>
```

包装器应调用与当前 SDK 配套的 `parameter.txt`、MiniLoader、afptool、
rkImageMaker/resource 工具，并在第二个参数指定的位置生成非空 `update.img`。
`merged` 与 `vendor-resource` 是互斥激活路径，不能再由 U-Boot 或其他脚本重复加载
同一 overlay。

如未设置 `RK_PACK_SCRIPT`，固件目录包含：

```
output/firmware/boot.img
output/firmware/idbloader.img
output/firmware/u-boot.itb
output/firmware/rootfs.img
```

设置且成功执行 vendor 包装器后，另有：

```
output/firmware/update.img
```

可通过 `MKIMAGE`、`FDTOVERLAY` 环境变量覆盖工具路径，但不得以通用 U-Boot
multi-image 冒充 Rockchip vendor resource/update.img 格式。

快速 TFTP 启动同样默认推送 base DTB；如需测试上述已核对并合并的 DTB，应显式
指定：

```bash
TFTP_DTB=output/firmware/rk3576-lubancat3-ocr.dtb \
  bash scripts/deploy/tftp_push.sh
```

### 11.2 烧录到 eMMC

优先使用项目的安全烧录入口。若存在 vendor 包装器生成的 `update.img`：

```bash
cd "${PROJ_DIR}"
export UPGRADE_TOOL=/absolute/path/to/upgrade_tool
bash scripts/deploy/flash_emmc.sh
```

若只有分区镜像，必须从当前 SDK 的 `parameter.txt` 读取并显式设置全部扇区偏移；
脚本会检查镜像范围不重叠后才调用 `upgrade_tool wl`：

```bash
export OFFSET_IDBLOADER=<parameter.txt中的扇区>
export OFFSET_UBOOT=<parameter.txt中的扇区>
export OFFSET_BOOT=<parameter.txt中的扇区>
export OFFSET_ROOTFS=<parameter.txt中的扇区>
bash scripts/deploy/flash_emmc.sh
```

该入口不会因找不到 USB 工具而自动回退到 `dd`。只有明确设置
`FLASH_METHOD=dd`、`LOCAL_DEVICE` 和全部 `OFFSET_*`，并在交互确认设备路径后，
才允许本地块设备写入。不要把文档中的示例偏移用于其他 SDK 或板卡版本。

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
ls -l /sys/class/backlight/
cat /sys/class/backlight/panel-backlight/brightness

# 2. 摄像头 (出厂已验证)
v4l2-ctl --device=/dev/video0 --list-formats-ext
v4l2-ctl --device=/dev/video0 --stream-mmap --stream-count=1

# 3. 按名称定位按键事件节点，再用 evtest 确认 KEY_CAMERA
for name in /sys/class/input/event*/device/name; do
  [ "$(cat "$name")" = "OCR GPIO Keys" ] || continue
  event="$(basename "$(dirname "$(dirname "$name")")")"
  echo "/dev/input/$event"
done
# 将上面输出的节点传给 evtest；不要假定它是 event0

# 4～6. 按 IIO name 定位，不依赖动态编号
find_iio() {
  for d in /sys/bus/iio/devices/iio:device*; do
    [ -r "$d/name" ] && [ "$(cat "$d/name")" = "$1" ] && {
      printf '%s\n' "$d"; return 0;
    }
  done
  return 1
}
AP3216C="$(find_iio ap3216c)" || exit 1
ICM42688="$(find_iio icm42688)" || exit 1
ADT7410="$(find_iio adt7410)" || exit 1
cat "$AP3216C/in_illuminance_raw"
cat "$ICM42688/in_accel_x_raw"
cat "$ADT7410/in_temp_raw" "$ADT7410/in_temp_scale" "$ADT7410/in_temp_offset"

# 7. 定位带 ocr,fan-pwm 标记的动态 pwmchip；应用启动后在另一终端检查 pwm0
for chip in /sys/class/pwm/pwmchip*; do
  [ -e "$chip/device/of_node/ocr,fan-pwm" ] && echo "$chip"
done
# cat <上面输出>/pwm0/period <上面输出>/pwm0/duty_cycle <上面输出>/pwm0/enable

# 8. RKNN 运行时
ls /usr/lib/librknnrt.so
cd /usr/share/ocr/models && sha256sum -c model_artifacts.sha256

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

以下命令只汇总项目统一入口；模型资产变量必须使用第 9 节中的真实路径：

```bash
# ===== 统一环境 =====
SDK_ROOT=/path/to/vendor-sdk
PROJ_DIR=/path/to/OCR/project

export UBOOT_SRC="${SDK_ROOT}/u-boot"
export KERNEL_SRC="${SDK_ROOT}/kernel-6.1"
export BUILDROOT_SRC="${SDK_ROOT}/buildroot"
export ARCH=arm64
export CROSS_COMPILE="${SDK_ROOT}/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-2021.07-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"
export SYSROOT=/path/to/vendor-buildroot-output/staging
export READELF="${CROSS_COMPILE}readelf"

cd "${PROJ_DIR}"

# ===== 依次生成统一 output/ 产物 =====
bash scripts/build/build_uboot.sh
bash scripts/build/build_kernel.sh
bash scripts/build/build_app.sh

# 设置 DET_*/REC_*/TRANS_* 真实模型与词表变量后：
bash scripts/build/build_models.sh

# 若 vendor rootfs 不含 Noto CJK，先设置 FONT_FILE。
bash scripts/build/build_rootfs.sh

# 安全默认：base DTB、分区镜像集合、不生成伪 update.img。
unset OCR_DTBO_LIST
unset DT_OVERLAY_MODE
unset RK_PACK_SCRIPT
bash scripts/build/make_image.sh

# 只有硬件核对完成后，才按第 11 节选择 merged 或 vendor-resource。
```

---

> **注意**: 本文档中的 defconfig 名称、DTS 文件名、GPIO 引脚号等需要根据实际 SDK 检出后的文件确认。
> 在 SDK `repo sync -l` 完成后，运行 `find kernel-6.1/arch/arm64/boot/dts/rockchip/ -name "*lubancat*"` 确认 DTS 文件名，
> 运行 `ls device/rockchip/rk3576/` 确认板级配置名称。构建与板端验收的真实边界另见
> `docs/07-测试/08-当前实现状态与板端验收清单.md`。
