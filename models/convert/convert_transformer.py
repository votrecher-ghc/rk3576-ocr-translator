#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
将轻量级翻译模型 (lite_transformer) 转换为 RKNN 格式。

转换配置:
  - 量化方式: FP16 (翻译模型对精度敏感, 不做 INT8 量化)
  - 目标平台: rk3576
  - 动态 shape 处理: 限定最大序列长度 (max_seq_len), 将变长输入固定为定长
    (RKNN 对动态 shape 支持有限, 采用 padding 到 max_seq_len 策略)
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
    "TRANS_ONNX_URL",
    "https://example.com/models/lite_transformer.onnx"
)
ONNX_PATH = os.environ.get("TRANS_ONNX_PATH",
                            os.path.join(SCRIPT_DIR, "lite_transformer.onnx"))

# RKNN 输出路径
RKNN_OUTPUT = os.path.join(OUTPUT_DIR, "lite_transformer_fp16.rknn")

# RKNN 配置
RKNN_TARGET = "rk3576"    # 目标平台

# 模型输入配置
MAX_SEQ_LEN = 128         # 最大序列长度 (与主配置 ocr_translator.json 一致)
VOCAB_SIZE = 32000        # 词表大小
# 翻译模型输入通常为 token id 序列
# input_shape: [1, MAX_SEQ_LEN] (batch=1, seq_len 固定为最大值)
INPUT_SHAPE = [[1, MAX_SEQ_LEN]]

MEAN_VALUES = []
STD_VALUES = []


def log(msg: str) -> None:
    """统一日志输出"""
    print(f"[TransConvert] {msg}")


def err(msg: str) -> None:
    """错误输出并退出"""
    print(f"[TransConvert ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------- 步骤1: 下载 ONNX 模型 ----------------------
def download_onnx() -> None:
    """下载翻译模型 ONNX 文件"""
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


# ---------------------- 步骤2: 处理动态 shape ----------------------
def handle_dynamic_shape() -> None:
    """
    处理动态 shape: 将变长序列固定为 max_seq_len.

    策略:
      - 使用 ONNX 的形状推断, 将 batch 和 seq_len 固定
      - 推理时对不足 max_seq_len 的输入做 padding (补 0 / pad token)
      - 对超出 max_seq_len 的输入做截断
    """
    log(f"处理动态 shape: 固定 seq_len = {MAX_SEQ_LEN}")
    log(f"  - batch_size = 1 (固定)")
    log(f"  - max_seq_len = {MAX_SEQ_LEN}")
    log(f"  - vocab_size = {VOCAB_SIZE}")
    log("  - 推理时对短输入做 padding, 长输入做截断")
    # 实际的 shape 固化在 convert_to_rknn 中通过 RKNN config 完成


# ---------------------- 步骤3: RKNN 转换 (FP16) ----------------------
def convert_to_rknn() -> None:
    """调用 rknn-toolkit2 将 ONNX 转换为 RKNN (FP16)"""
    try:
        from rknn.api import RKNN  # noqa: WPS433
    except ImportError:
        err("未安装 rknn-toolkit, 请先安装: pip install rknn-toolkit2")

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    rknn = RKNN()

    # 配置模型: FP16 量化, 固定输入 shape
    rknn.config(
        mean_values=MEAN_VALUES if MEAN_VALUES else None,
        std_values=STD_VALUES if STD_VALUES else None,
        target_platform=RKNN_TARGET,
        quantized_dtype="w16a16",   # FP16 量化
        optimization_level=3,
    )

    # 加载 ONNX 模型 (指定输入 shape 以固化动态维度)
    log(f"加载 ONNX 模型: {ONNX_PATH}")
    log(f"  输入 shape: {INPUT_SHAPE}")
    ret = rknn.load_onnx(
        model=ONNX_PATH,
        inputs=["input"],          # 输入节点名 (按实际模型调整)
        input_size_list=INPUT_SHAPE,
    )
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
    parser = argparse.ArgumentParser(description="翻译模型转 RKNN (FP16)")
    parser.add_argument("--skip-download", action="store_true",
                        help="跳过 ONNX 下载步骤 (已手动准备)")
    parser.add_argument("--max-seq-len", type=int, default=MAX_SEQ_LEN,
                        help=f"最大序列长度 (默认 {MAX_SEQ_LEN})")
    args = parser.parse_args()

    # 覆盖全局 max_seq_len
    global MAX_SEQ_LEN, INPUT_SHAPE
    MAX_SEQ_LEN = args.max_seq_len
    INPUT_SHAPE = [[1, MAX_SEQ_LEN]]

    log("=== 翻译模型转换开始 ===")
    if not args.skip_download:
        download_onnx()
    handle_dynamic_shape()
    convert_to_rknn()
    verify_output()
    log("=== 翻译模型转换结束 ===")


if __name__ == "__main__":
    main()
