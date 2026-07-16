#!/usr/bin/env python3
"""Convert a static encoder/decoder translation pair to RKNN.

The runtime intentionally accepts only this contract:
  encoder: token ids [1, source_length] -> encoder state
  decoder: token ids [1, target_length] + encoder state -> logits

Dynamic ONNX dimensions are rejected. Export the source models with fixed
sequence lengths before running this converter.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
from typing import Iterable, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_OUTPUT_DIR = Path(
    os.environ.get("MODEL_OUTPUT_DIR", PROJECT_ROOT / "output" / "models")
)
MAX_RUNTIME_TOKENS = 256
MAX_RUNTIME_VOCAB = 32000
MAX_RUNTIME_TOKEN_BYTES = 255
TOKENIZER_CONTRACT = "greedy-vocab-v1"


def log(message: str) -> None:
    print(f"[TransConvert] {message}")


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"[TransConvert ERROR] {message}")


def require_file(value: str | None, label: str) -> Path:
    if not value:
        fail(f"缺少 {label}；请通过命令行参数或对应环境变量指定本地文件")
    path = Path(value).expanduser().resolve()
    if not path.is_file():
        fail(f"{label} 不存在或不是普通文件: {path}")
    return path


def external_inputs(graph: object) -> list[object]:
    initializer_names = {item.name for item in graph.initializer}
    return [item for item in graph.input if item.name not in initializer_names]


def tensor_shape(value_info: object, label: str) -> tuple[int, ...]:
    tensor_type = value_info.type.tensor_type
    if not tensor_type.HasField("shape"):
        fail(f"{label} 没有 tensor shape")
    dims: list[int] = []
    for index, dim in enumerate(tensor_type.shape.dim):
        if not dim.HasField("dim_value") or dim.dim_value <= 0:
            fail(f"{label} 第 {index} 维是动态维度；请先导出静态 ONNX")
        dims.append(int(dim.dim_value))
    if not dims:
        fail(f"{label} 不能是标量")
    return tuple(dims)


def count_vocab(path: Path, label: str) -> int:
    try:
        tokens = path.read_text(encoding="utf-8-sig").splitlines()
    except (OSError, UnicodeError) as exc:
        fail(f"无法读取 {label}: {exc}")
    if not tokens or any("\x00" in token for token in tokens):
        fail(f"{label} 为空或格式无效: {path}")
    if len(tokens) > MAX_RUNTIME_VOCAB:
        fail(f"{label} 超过运行时上限 {MAX_RUNTIME_VOCAB}")
    for index, token in enumerate(tokens):
        if len(token.encode("utf-8")) > MAX_RUNTIME_TOKEN_BYTES:
            fail(f"{label} 第 {index} 个 token 超过 "
                 f"{MAX_RUNTIME_TOKEN_BYTES} UTF-8 字节")
    required_groups: Sequence[Iterable[str]] = (
        ("<pad>", "[PAD]", "<PAD>"),
        ("<s>", "<bos>", "[BOS]", "<BOS>", "[CLS]"),
        ("</s>", "<eos>", "[EOS]", "<EOS>", "[SEP]"),
        ("<unk>", "[UNK]", "<UNK>"),
    )
    token_set = set(tokens)
    if any(not any(name in token_set for name in group)
           for group in required_groups):
        fail(f"{label} 必须包含 PAD/BOS/EOS/UNK 特殊 token")
    return len(tokens)


def validate_models(encoder_path: Path, decoder_path: Path,
                    target_vocab_size: int) -> None:
    try:
        import onnx
        from onnx import TensorProto, checker
    except ImportError:
        fail("未安装 onnx；请在 RKNN 转换环境中安装 onnx")

    try:
        encoder = onnx.load(str(encoder_path), load_external_data=True)
        decoder = onnx.load(str(decoder_path), load_external_data=True)
        checker.check_model(encoder)
        checker.check_model(decoder)
    except Exception as exc:
        fail(f"ONNX 校验失败: {exc}")

    enc_inputs = external_inputs(encoder.graph)
    enc_outputs = list(encoder.graph.output)
    dec_inputs = external_inputs(decoder.graph)
    dec_outputs = list(decoder.graph.output)
    if len(enc_inputs) != 1 or len(enc_outputs) != 1:
        fail("编码器必须恰好有 1 个外部输入和 1 个输出")
    if len(dec_inputs) != 2 or len(dec_outputs) != 1:
        fail("解码器必须恰好有 2 个外部输入和 1 个输出")

    token_types = {TensorProto.INT32, TensorProto.INT64}
    enc_type = enc_inputs[0].type.tensor_type.elem_type
    enc_shape = tensor_shape(enc_inputs[0], "编码器 token 输入")
    if enc_type not in token_types or len(enc_shape) != 2 or enc_shape[0] != 1:
        fail("编码器输入必须是静态 [1, sequence] INT32/INT64")
    if enc_shape[1] > MAX_RUNTIME_TOKENS:
        fail(f"编码器序列长度不能超过 {MAX_RUNTIME_TOKENS}")

    token_candidates = []
    for value in dec_inputs:
        shape = tensor_shape(value, f"解码器输入 {value.name}")
        if (value.type.tensor_type.elem_type in token_types and
                len(shape) == 2 and shape[0] == 1):
            token_candidates.append((value, shape))
    if len(token_candidates) != 1:
        fail("解码器必须能唯一识别一个 [1, sequence] INT32/INT64 token 输入")
    token_input, token_shape = token_candidates[0]
    if token_shape[1] <= 1 or token_shape[1] > MAX_RUNTIME_TOKENS:
        fail(f"解码器序列长度必须在 2..{MAX_RUNTIME_TOKENS} 范围内")

    state_input = next(value for value in dec_inputs if value is not token_input)
    enc_state_shape = tensor_shape(enc_outputs[0], "编码器 state 输出")
    dec_state_shape = tensor_shape(state_input, "解码器 state 输入")
    if enc_state_shape != dec_state_shape:
        fail(f"编码器输出 {enc_state_shape} 与解码器 state 输入 "
             f"{dec_state_shape} 不一致")

    logits_shape = tensor_shape(dec_outputs[0], "解码器 logits 输出")
    if (len(logits_shape) < 2 or logits_shape[0] != 1 or
            logits_shape[-1] != target_vocab_size):
        fail("解码器 logits 必须以目标词表大小作为最后一维")
    steps = 1
    for dim in logits_shape[1:-1]:
        steps *= dim
    if steps != 1 and steps < token_shape[1]:
        fail("解码器 logits 的时间步数不足以覆盖目标序列")

    log(f"接口校验通过: src_len={enc_shape[1]}, "
        f"tgt_len={token_shape[1]}, vocab={target_vocab_size}")


def convert_one(source: Path, destination: Path, target: str, label: str) -> None:
    try:
        from rknn.api import RKNN
    except ImportError:
        fail("未安装 rknn-toolkit2；请使用 Rockchip 支持的转换环境")

    destination.parent.mkdir(parents=True, exist_ok=True)
    rknn = RKNN(verbose=False)
    try:
        log(f"配置 {label}: target={target}, FP16")
        ret = rknn.config(target_platform=target, optimization_level=3)
        if ret != 0:
            fail(f"{label} RKNN 配置失败，错误码 {ret}")
        ret = rknn.load_onnx(model=str(source))
        if ret != 0:
            fail(f"{label} ONNX 加载失败，错误码 {ret}")
        ret = rknn.build(do_quantization=False)
        if ret != 0:
            fail(f"{label} RKNN 构建失败，错误码 {ret}")
        ret = rknn.export_rknn(str(destination))
        if ret != 0:
            fail(f"{label} RKNN 导出失败，错误码 {ret}")
    finally:
        rknn.release()

    if not destination.is_file() or destination.stat().st_size < 1024:
        fail(f"{label} 输出缺失或异常小: {destination}")
    log(f"已生成 {destination} ({destination.stat().st_size / 1048576:.2f} MiB)")


def copy_vocab(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if source != destination.resolve():
        shutil.copy2(source, destination)
    log(f"已准备词表 {destination}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="将静态 encoder/decoder 翻译 ONNX 转为 RKNN FP16 模型"
    )
    parser.add_argument("--encoder-onnx", default=os.environ.get(
        "TRANS_ENCODER_ONNX_PATH"))
    parser.add_argument("--decoder-onnx", default=os.environ.get(
        "TRANS_DECODER_ONNX_PATH"))
    parser.add_argument("--src-vocab", default=os.environ.get(
        "TRANS_SRC_VOCAB_PATH"))
    parser.add_argument("--tgt-vocab", default=os.environ.get(
        "TRANS_TGT_VOCAB_PATH"))
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--target", default=os.environ.get(
        "RKNN_TARGET", "rk3576"))
    parser.add_argument("--quantize", choices=("fp16",), default="fp16",
                        help="兼容统一转换命令；翻译模型仅支持 fp16")
    parser.add_argument("--tokenizer-contract", default=os.environ.get(
        "TRANS_TOKENIZER_CONTRACT"),
        help=f"必须显式指定 {TOKENIZER_CONTRACT}")
    parser.add_argument("--src-lang", default=os.environ.get("TRANS_SRC_LANG"))
    parser.add_argument("--tgt-lang", default=os.environ.get("TRANS_TGT_LANG"))
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.tokenizer_contract != TOKENIZER_CONTRACT:
        fail("当前板端 tokenizer 不是通用 BPE/SentencePiece；只有专门按 "
             f"{TOKENIZER_CONTRACT} 导出的模型可部署")
    if not args.src_lang or not args.tgt_lang:
        fail("必须用 --src-lang/--tgt-lang 声明模型的固定语言对")
    encoder = require_file(args.encoder_onnx, "编码器 ONNX")
    decoder = require_file(args.decoder_onnx, "解码器 ONNX")
    src_vocab = require_file(args.src_vocab, "源语言词表")
    tgt_vocab = require_file(args.tgt_vocab, "目标语言词表")
    output_dir = args.output_dir.expanduser().resolve()

    count_vocab(src_vocab, "源语言词表")
    target_vocab_size = count_vocab(tgt_vocab, "目标语言词表")
    validate_models(encoder, decoder, target_vocab_size)

    convert_one(encoder, output_dir / "lite_transformer_encoder_fp16.rknn",
                args.target, "编码器")
    convert_one(decoder, output_dir / "lite_transformer_decoder_fp16.rknn",
                args.target, "解码器")
    copy_vocab(src_vocab, output_dir / "translation_src_vocab.txt")
    copy_vocab(tgt_vocab, output_dir / "translation_tgt_vocab.txt")
    manifest = {
        "contract": "static-encoder-decoder-v1",
        "tokenizer": TOKENIZER_CONTRACT,
        "src_lang": args.src_lang,
        "tgt_lang": args.tgt_lang,
        "max_vocab": MAX_RUNTIME_VOCAB,
        "max_token_bytes": MAX_RUNTIME_TOKEN_BYTES,
    }
    (output_dir / "translation_manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    log("翻译模型转换完成")


if __name__ == "__main__":
    main()
