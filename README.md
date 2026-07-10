# RK3576 端侧 OCR 实时翻译系统

基于鲁班猫3 RK3576 开发板的嵌入式 Linux OCR 翻译设备，支持实时预览翻译与按键拍照翻译两种模式。

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
# 1. 构建 BSP (需先获取野火 SDK)
./scripts/build/build_kernel.sh

# 2. 构建应用
cd app && mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-linux.cmake
make -j$(nproc)

# 3. 转换 RKNN 模型 (PC 端)
cd models/convert
python convert_ppocr_det.py
python convert_ppocr_rec.py
python convert_transformer.py

# 4. 部署到板卡
./scripts/deploy/tftp_push.sh
```

## 文档

完整文档位于 `docs/` 目录，详见 `docs/00-总览/02-文档导读与版本变更.md`。

## 许可证

私有项目，版权所有。
