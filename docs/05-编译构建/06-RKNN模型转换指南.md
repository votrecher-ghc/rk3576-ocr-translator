# RKNN模型转换指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 环境准备](#2-环境准备)
- [3. 模型转换流程](#3-模型转换流程)
- [4. 量化与校准](#4-量化与校准)
- [5. 模型验证](#5-模型验证)

---

## 1. 概述

使用 RKNN Toolkit2 将 PyTorch/ONNX 训练的 OCR 模型转换为 RK3576 NPU 可执行的 RKNN 格式。

## 2. 环境准备

```bash
pip3 install rknn-toolkit2
pip3 install torch torchvision onnx
```

## 3. 模型转换流程

```python
from rknn.api import RKNN

rknn = RKNN()

# 配置
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform='rk3576',
    quantized_dtype='w8a8',
)

# 加载 ONNX 模型
rknn.load_onnx(model='ocr_det.onnx')

# 构建（含量化）
rknn.build(do_quantization=True, dataset='calib_imgs.txt')

# 导出
rknn.export_rknn('ocr_det.rknn')
```

## 4. 量化与校准

- 准备 100-500 张代表性校准图像
- 量化精度：INT8（w8a8）
- 关注量化后精度损失（应 < 2%）

## 5. 模型验证

```python
# PC 端模拟验证
rknn.init_runtime(target=None)  # simulator
outputs = rknn.inference(inputs=[img])

# 板端验证
# 将 .rknn 文件拷贝到板子，使用 rknn_api 调用
```

---

> 相关文档：[02-模块详细设计/04-OCR与离线翻译.md](../02-模块详细设计/04-OCR与离线翻译.md)
