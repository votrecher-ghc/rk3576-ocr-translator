#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
将 PaddleOCR 文本检测模型 (ppocrv4_det) 转换为 RKNN 格式。

转换流程:
  1. 下载 PaddleOCR ppocrv4_det 的 ONNX 模型
  2. 构建 RKNN 配置: target=rk3576, quantization=INT8, 量化数据集=det_dataset.txt
  3. 调用 rknn-toolkit2 执行转换并导出 .rknn 文件
"""

import os
import sys
import argparse
import urllib.request

# ---------------------- 路径与默认值配置 ----------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
OUTPUT_DIR = os.environ.get("MODEL_OUTPUT_DIR",
                            os.path.join(PROJECT_ROOT, "output", "models"))

# 模型源文件 (ONNX)
ONNX_URL = os.environ.get(
    "DET_ONNX_URL",
    "https://paddleocr.bj.bcebos.com/PP-OCRv4/chinese/ch_PP-OCRv4_det_infer.onnx"
)
ONNX_PATH = os.environ.get("DET_ONNX_PATH",
                            os.path.join(SCRIPT_DIR, "ch_PP-OCRv4_det_infer.onnx"))

# RKNN 输出路径
RKNN_OUTPUT = os.path.join(OUTPUT_DIR, "ppocrv4_det_int8.rknn")

# RKNN 配置
RKNN_TARGET = "rk3576"          # 目标平台
RKNN_QUANT_DTYPE = "w8a8"       # INT8 量化
QUANT_DATASET = os.path.join(SCRIPT_DIR, "det_dataset.txt")  # 量化校准数据集列表
# 量化图片所在目录 (需提前准备一批 OCR 场景图)
QUANT_IMG_DIR = os.environ.get(
    "DET_QUANT_IMG_DIR", os.path.join(SCRIPT_DIR, "quant_images", "det")
)

# 模型输入配置
INPUT_WIDTH = 960
INPUT_HEIGHT = 960
MEAN_VALUES = [[0.485, 0.456, 0.406]]
STD_VALUES = [[0.229, 0.224, 0.225]]


def log(msg: str) -> None:
    """统一日志输出"""
    print(f"[DetConvert] {msg}")


def err(msg: str) -> None:
    """错误输出并退出"""
    print(f"[DetConvert ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------- 步骤1: 下载 ONNX 模型 ----------------------
def download_onnx() -> None:
    """下载 PaddleOCR 检测模型 ONNX 文件"""
    if os.path.exists(ONNX_PATH):
        log(f"ONNX 模型已存在, 跳过下载: {ONNX_PATH}")
        return

    log(f"下载 ONNX 模型: {ONNX_URL}")
    os.makedirs(os.path.dirname(ONNX_PATH), exist_ok=True)
    try:
        urllib.request.urlretrieve(ONNX_URL, ONNX_PATH)
        log("下载完成")
    except Exception as e:
        err(f"下载失败: {e}")


# ---------------------- 步骤2: 构建量化数据集列表 ----------------------
def build_quant_dataset() -> None:
    """扫描量化图片目录, 生成 det_dataset.txt (每行一个图片路径)"""
    log(f"构建量化数据集列表: {QUANT_DATASET}")

    if not os.path.isdir(QUANT_IMG_DIR):
        log(f"警告: 量化图片目录不存在 ({QUANT_IMG_DIR}), 将创建空数据集")
        os.makedirs(QUANT_IMG_DIR, exist_ok=True)

    img_exts = (".jpg", ".jpeg", ".png", ".bmp")
    lines = []
    for name in sorted(os.listdir(QUANT_IMG_DIR)):
        if name.lower().endswith(img_exts):
            lines.append(os.path.join(QUANT_IMG_DIR, name))

    with open(QUANT_DATASET, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    if not lines:
        log("警告: 未找到量化图片, 转换将无法正确量化, 请补充校准图片")
    else:
        log(f"共 {len(lines)} 张校准图片")


# ---------------------- 步骤3: RKNN 转换 ----------------------
def convert_to_rknn() -> None:
    """调用 rknn-toolkit2 将 ONNX 转换为 RKNN"""
    try:
        from rknn.api import RKNN  # noqa: WPS433
    except ImportError:
        err("未安装 rknn-toolkit, 请先安装: pip install rknn-toolkit2")

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # 构建 RKNN 配置字典
    rknn = RKNN()

    # 配置模型: INT8 量化
    rknn.config(
        mean_values=MEAN_VALUES,
        std_values=STD_VALUES,
        target_platform=RKNN_TARGET,
        quantized_dtype=RKNN_QUANT_DTYPE,
        quantized_method="channel",
        quantized_algorithm="normal",
        optimization_level=3,
    )

    # 加载 ONNX 模型
    log(f"加载 ONNX 模型: {ONNX_PATH}")
    ret = rknn.load_onnx(model=ONNX_PATH)
    if ret != 0:
        err(f"加载 ONNX 失败, 错误码: {ret}")

    # 构建 RKNN 模型 (使用量化数据集)
    log(f"构建 RKNN (量化数据集: {QUANT_DATASET})")
    ret = rknn.build(do_quantization=True, dataset=QUANT_DATASET)
    if ret != 0:
        err(f"构建 RKNN 失败, 错误码: {ret}")

    # 导出 RKNN 模型
    log(f"导出 RKNN 模型: {RKNN_OUTPUT}")
    ret = rknn.export_rknn(RKNN_OUTPUT)
    if ret != 0:
        err(f"导出 RKNN 失败, 错误码: {ret}")

    rknn.release()
    log("转换完成")


# ---------------------- 步骤4: 校验输出 ----------------------
def verify_output() -> None:
    """检查输出文件是否存在"""
    if not os.path.exists(RKNN_OUTPUT):
        err(f"未找到输出文件: {RKNN_OUTPUT}")
    size = os.path.getsize(RKNN_OUTPUT)
    if size < 1024:
        err(f"输出文件过小 ({size} 字节), 转换可能异常")
    log(f"输出文件: {RKNN_OUTPUT} ({size / 1024 / 1024:.2f} MB)")


# ---------------------- 主流程 ----------------------
def main() -> None:
    parser = argparse.ArgumentParser(description="PaddleOCR det 模型转 RKNN (INT8)")
    parser.add_argument("--skip-download", action="store_true",
                        help="跳过 ONNX 下载步骤 (已手动准备)")
    args = parser.parse_args()

    log("=== 检测模型转换开始 ===")
    if not args.skip_download:
        download_onnx()
    build_quant_dataset()
    convert_to_rknn()
    verify_output()
    log("=== 检测模型转换结束 ===")


if __name__ == "__main__":
    main()
