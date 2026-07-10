#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
将 PaddleOCR 文本识别模型 (ppocrv4_rec) 转换为 RKNN 格式。

转换配置:
  - 量化方式: FP16 (保持精度, 识别模型对精度敏感)
  - 目标平台: rk3576
  - 输入尺寸: 320x48 (PaddleOCR rec 默认)
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
    "REC_ONNX_URL",
    "https://paddleocr.bj.bcebos.com/PP-OCRv4/chinese/ch_PP-OCRv4_rec_infer.onnx"
)
ONNX_PATH = os.environ.get("REC_ONNX_PATH",
                           os.path.join(SCRIPT_DIR, "ch_PP-OCRv4_rec_infer.onnx"))

# RKNN 输出路径
RKNN_OUTPUT = os.path.join(OUTPUT_DIR, "ppocrv4_rec_fp16.rknn")

# RKNN 配置
RKNN_TARGET = "rk3576"    # 目标平台

# 模型输入配置 (rec 模型: 宽度动态, 高度固定 48)
INPUT_WIDTH = 320
INPUT_HEIGHT = 48
MEAN_VALUES = [[0.5, 0.5, 0.5]]
STD_VALUES = [[0.5, 0.5, 0.5]]


def log(msg: str) -> None:
    """统一日志输出"""
    print(f"[RecConvert] {msg}")


def err(msg: str) -> None:
    """错误输出并退出"""
    print(f"[RecConvert ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------- 步骤1: 下载 ONNX 模型 ----------------------
def download_onnx() -> None:
    """下载 PaddleOCR 识别模型 ONNX 文件"""
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


# ---------------------- 步骤2: RKNN 转换 (FP16) ----------------------
def convert_to_rknn() -> None:
    """调用 rknn-toolkit2 将 ONNX 转换为 RKNN (FP16)"""
    try:
        from rknn.api import RKNN  # noqa: WPS433
    except ImportError:
        err("未安装 rknn-toolkit, 请先安装: pip install rknn-toolkit2")

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    rknn = RKNN()

    # 配置模型: FP16 量化 (识别模型保持精度)
    rknn.config(
        mean_values=MEAN_VALUES,
        std_values=STD_VALUES,
        target_platform=RKNN_TARGET,
        quantized_dtype="w16a16",   # FP16 量化
        optimization_level=3,
    )

    # 加载 ONNX 模型
    log(f"加载 ONNX 模型: {ONNX_PATH}")
    ret = rknn.load_onnx(model=ONNX_PATH)
    if ret != 0:
        err(f"加载 ONNX 失败, 错误码: {ret}")

    # 构建 RKNN 模型 (FP16 不需要量化数据集)
    log("构建 RKNN (FP16, 无需量化数据集)")
    ret = rknn.build(do_quantization=False)
    if ret != 0:
        err(f"构建 RKNN 失败, 错误码: {ret}")

    # 导出 RKNN 模型
    log(f"导出 RKNN 模型: {RKNN_OUTPUT}")
    ret = rknn.export_rknn(RKNN_OUTPUT)
    if ret != 0:
        err(f"导出 RKNN 失败, 错误码: {ret}")

    rknn.release()
    log("转换完成")


# ---------------------- 步骤3: 校验输出 ----------------------
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
    parser = argparse.ArgumentParser(description="PaddleOCR rec 模型转 RKNN (FP16)")
    parser.add_argument("--skip-download", action="store_true",
                        help="跳过 ONNX 下载步骤 (已手动准备)")
    args = parser.parse_args()

    log("=== 识别模型转换开始 ===")
    if not args.skip_download:
        download_onnx()
    convert_to_rknn()
    verify_output()
    log("=== 识别模型转换结束 ===")


if __name__ == "__main__":
    main()
