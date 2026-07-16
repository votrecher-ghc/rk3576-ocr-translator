# RK3576 端侧 OCR 实时翻译系统

基于鲁班猫3 RK3576 开发板的嵌入式 Linux OCR 翻译设备，支持实时预览翻译与按键拍照翻译两种模式。

> **最后更新：2026-07-16**
>
> 项目侧软件主链已经从骨架补齐；vendor SDK 交叉编译、真实模型数值验证和实板
> 联调仍未完成。当前状态不能等同于量产验收通过，详见
> [`IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md)。

## 硬件平台

| 项目 | 规格 |
|------|------|
| 主控 | RK3576 (Cortex-A72 + Cortex-A53 + Mali-G52 + 6T NPU) |
| 内存 | LPDDR4/4X 4GB |
| 存储 | eMMC 32GB |
| 内核 | Linux 6.1 (lbc-develop-6.1) |
| 系统 | Buildroot 构建的最小 Linux |

## 功能特性

- **实时预览翻译**：摄像头持续采集 → NPU 文字检测/识别 → 本地离线翻译 → DRM/KMS 叠加显示
- **按键拍照翻译**：按键触发抓拍 → OCR → 翻译 → 按时间戳归档
- **电子防抖**：ICM42688 姿态融合 + RGA 图像补偿
- **自适应亮度**：AP3216C 环境光感知 → 动态背光调节
- **主动散热**：ADT7410 温度监测 → PWM 风扇回差控制

## 技术栈

C/C++、Buildroot、Device Tree、I2C、SPI、PWM、V4L2、RGA、DRM/KMS、RKNN

## 目录结构

```
project/
├── bsp/          # 板级支持包 (U-Boot/Kernel/Buildroot/DTS)
├── app/          # 主应用程序 (C/C++)
├── models/       # RKNN 模型资产
├── config/       # 运行时配置
├── scripts/      # 构建/部署/调试脚本
├── tools/        # 测试工具
├── tests/        # 测试用例
└── docs/         # 完整文档体系
```

## 快速开始

```bash
# 1. 指定野火/Rockchip SDK（默认不会下载，也不会混用上游 Buildroot）
export UBOOT_SRC=/path/to/vendor-sdk/u-boot
export KERNEL_SRC=/path/to/vendor-sdk/kernel
export BUILDROOT_SRC=/path/to/vendor-sdk/buildroot
export SYSROOT=/path/to/vendor-sdk/sysroot
export CROSS_COMPILE=/path/to/vendor-sdk/toolchain/bin/aarch64-buildroot-linux-gnu-

# build_kernel.sh 仅在显式设置 ALLOW_SOURCE_DOWNLOAD=1 时允许获取缺失源码；
# 正式构建应使用已经固定版本的本地 vendor SDK。

# 构建 BSP
bash scripts/build/build_uboot.sh
bash scripts/build/build_kernel.sh

# 2. 构建应用（统一收集到 output/app/ocr_translator，供 rootfs 使用）
bash scripts/build/build_app.sh

# 可在普通 Linux 主机上只构建并运行硬件无关单元测试
cmake -S app -B output/test-build \
  -DOCR_BUILD_HARDWARE_APP=OFF -DBUILD_TESTING=ON
cmake --build output/test-build --parallel
ctest --test-dir output/test-build --output-on-failure

# 3. 转换 RKNN 模型 (PC 端；路径均须指向与运行时匹配的本地资产)
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
bash scripts/build/build_models.sh
# output/models/model_artifacts.sha256 会固定本次部署的模型、词表与 manifest

# 4. 构建 rootfs（若 vendor rootfs 未带 Noto CJK，另设置 FONT_FILE）
export FONT_FILE=/path/to/NotoSansCJK-Regular.ttc
bash scripts/build/build_rootfs.sh

# 5. 部署到板卡
# 默认推送未合并 overlay 的 vendor base DTB
bash scripts/deploy/tftp_push.sh

# 只有逐项核对原理图、base DTS 与供电名称后，才显式选择 overlay 打包：
export OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-adt7410.dtbo ocr-pwm-fan.dtbo"
export DT_OVERLAY_MODE=merged
bash scripts/build/make_image.sh
# merged 只写入启动 DTB；vendor-resource 则必须设置 RK_PACK_SCRIPT，由 vendor 工具启用。
# 不要让两条路径重复加载同一 overlay。未核对 media graph 的 IMX415 overlay 默认跳过。
# 如需用 TFTP 启动合并后的 DTB：
TFTP_DTB=output/firmware/rk3576-lubancat3-ocr.dtb \
  bash scripts/deploy/tftp_push.sh
```

## 文档

完整文档位于 `docs/` 目录，详见 `docs/00-总览/02-文档导读与版本变更.md`。
本轮实现内容、设计约束与下一位开发者的接手顺序见
`IMPLEMENTATION_GUIDE.md`。
当前软件完成度、硬件边界与实板验收顺序见
`docs/07-测试/08-当前实现状态与板端验收清单.md`。

## 许可证

私有项目，版权所有。
