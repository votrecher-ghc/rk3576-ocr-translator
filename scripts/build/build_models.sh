#!/bin/bash
# =====================================================================
# RKNN 模型转换脚本 (统一入口)
# 功能: 运行 Python 转换脚本, 将 PaddleOCR / Transformer 模型转为 RKNN
# 输出: *.rknn 模型文件 (供板端 NPU 推理使用)
# =====================================================================
set -e

# ---------------------- 路径与工具链变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CONVERT_DIR="${PROJECT_ROOT}/models/convert"
OUT_DIR="${MODEL_OUTPUT_DIR:-${PROJECT_ROOT}/output/models}"
export MODEL_OUTPUT_DIR="${OUT_DIR}"
FINAL_DIR="/usr/share/ocr/models"

# 各转换脚本路径
CONVERT_DET="${CONVERT_DIR}/convert_ppocr_det.py"
CONVERT_REC="${CONVERT_DIR}/convert_ppocr_rec.py"
CONVERT_TRANS="${CONVERT_DIR}/convert_transformer.py"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Models $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[Models ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 环境检查 ----------------------
check_env() {
    command -v python3 >/dev/null 2>&1 || err "未找到 python3"
    command -v sha256sum >/dev/null 2>&1 || err "未找到 sha256sum"
    python3 -c "import rknn" 2>/dev/null || err "未检测到 rknn-toolkit2"
    [ -f "${CONVERT_DET}" ] || err "缺少转换脚本: ${CONVERT_DET}"
    [ -f "${CONVERT_REC}" ] || err "缺少转换脚本: ${CONVERT_REC}"
    [ -f "${CONVERT_TRANS}" ] || err "缺少转换脚本: ${CONVERT_TRANS}"
}

prepare_output() {
    mkdir -p "${OUT_DIR}"
    rm -f "${OUT_DIR}/ppocrv4_det_int8.rknn" \
          "${OUT_DIR}/ppocrv4_rec_fp16.rknn" \
          "${OUT_DIR}/ppocr_keys_v1.txt" \
          "${OUT_DIR}/lite_transformer_encoder_fp16.rknn" \
          "${OUT_DIR}/lite_transformer_decoder_fp16.rknn" \
          "${OUT_DIR}/translation_src_vocab.txt" \
          "${OUT_DIR}/translation_tgt_vocab.txt" \
          "${OUT_DIR}/translation_manifest.json" \
          "${OUT_DIR}/model_artifacts.sha256"
}

# ---------------------- 步骤2: 运行转换脚本 ----------------------
convert_det() {
    log "转换 PaddleOCR 检测模型 (INT8)"
    python3 "${CONVERT_DET}"
}

convert_rec() {
    log "转换 PaddleOCR 识别模型 (FP16)"
    python3 "${CONVERT_REC}"

    if [ -n "${OCR_REC_VOCAB_PATH:-}" ] && [ -f "${OCR_REC_VOCAB_PATH}" ]; then
        install -m 0644 "${OCR_REC_VOCAB_PATH}" "${OUT_DIR}/ppocr_keys_v1.txt"
    elif [ ! -f "${OUT_DIR}/ppocr_keys_v1.txt" ]; then
        err "请设置 OCR_REC_VOCAB_PATH，指向与识别模型匹配的 PaddleOCR 字符词表"
    fi
}

convert_trans() {
    log "转换翻译模型 (FP16)"
    [ -n "${TRANS_ENCODER_ONNX_PATH:-}" ] || \
        err "请设置 TRANS_ENCODER_ONNX_PATH"
    [ -n "${TRANS_DECODER_ONNX_PATH:-}" ] || \
        err "请设置 TRANS_DECODER_ONNX_PATH"
    [ -n "${TRANS_SRC_VOCAB_PATH:-}" ] || \
        err "请设置 TRANS_SRC_VOCAB_PATH"
    [ -n "${TRANS_TGT_VOCAB_PATH:-}" ] || \
        err "请设置 TRANS_TGT_VOCAB_PATH"
    [ "${TRANS_TOKENIZER_CONTRACT:-}" = "greedy-vocab-v1" ] || \
        err "请明确设置 TRANS_TOKENIZER_CONTRACT=greedy-vocab-v1"
    [ -n "${TRANS_SRC_LANG:-}" ] || err "请设置 TRANS_SRC_LANG"
    [ -n "${TRANS_TGT_LANG:-}" ] || err "请设置 TRANS_TGT_LANG"
    python3 "${CONVERT_TRANS}"
}

generate_checksums() {
    local artifacts=(
        ppocrv4_det_int8.rknn
        ppocrv4_rec_fp16.rknn
        ppocr_keys_v1.txt
        lite_transformer_encoder_fp16.rknn
        lite_transformer_decoder_fp16.rknn
        translation_src_vocab.txt
        translation_tgt_vocab.txt
        translation_manifest.json
    )
    (cd "${OUT_DIR}" && sha256sum "${artifacts[@]}" > model_artifacts.sha256) || \
        err "生成模型 SHA256 清单失败"
}

# ---------------------- 步骤3: 校验输出模型 ----------------------
check_outputs() {
    mkdir -p "${OUT_DIR}"
    log "检查转换后的 RKNN 模型文件"

    local required=(
        ppocrv4_det_int8.rknn
        ppocrv4_rec_fp16.rknn
        ppocr_keys_v1.txt
        lite_transformer_encoder_fp16.rknn
        lite_transformer_decoder_fp16.rknn
        translation_src_vocab.txt
        translation_tgt_vocab.txt
        translation_manifest.json
        model_artifacts.sha256
    )
    local name
    for name in "${required[@]}"; do
        [ -s "${OUT_DIR}/${name}" ] || err "缺少模型部署产物: ${OUT_DIR}/${name}"
        log "  已生成: ${name} ($(du -h "${OUT_DIR}/${name}" | cut -f1))"
    done

    log "全部模型与词表产物已生成"
    log "提示: 部署时请将模型拷贝到板端 ${FINAL_DIR}/ 目录"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== 模型转换开始 ==="
    check_env
    prepare_output
    convert_det
    convert_rec
    convert_trans
    generate_checksums
    check_outputs
    log "=== 模型转换结束 ==="
}

main "$@"
