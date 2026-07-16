#!/bin/sh
# =====================================================================
# post_build.sh - Buildroot 构建后处理脚本
# 功能: 在 rootfs 构建完成后, 拷贝运行时依赖到目标文件系统
# 由 Buildroot 在 make 完成后自动调用
# 参数: $1 = target 目录 (即 rootfs 根目录)
# =====================================================================
set -eu

# ---------------------- 路径变量 ----------------------
# Buildroot 传入的目标 rootfs 目录
TARGET_DIR="${1:-}"

# 项目根目录 (本脚本所在路径上溯)
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${BOARD_DIR}/../../../../.." && pwd)"

# 可选的显式 fallback。正常路径应由 vendor Buildroot 包把库安装进
# TARGET_DIR；不再从项目内的任意目录无条件覆盖 SDK 产物。
RKNN_LIB_DIR="${RKNN_LIB_DIR:-}"
RGA_LIB_DIR="${RGA_LIB_DIR:-}"

# 模型文件目录
MODEL_DIR="${MODEL_DIR:-${PROJECT_ROOT}/output/models}"
MODULE_ARCHIVE="${MODULE_ARCHIVE:-${PROJECT_ROOT}/output/kernel/modules.tar.gz}"
APP_BIN="${APP_BIN:-${PROJECT_ROOT}/output/app/ocr_translator}"
READELF="${READELF:-readelf}"

# 目标安装路径
TARGET_LIB_DIR="${TARGET_DIR}/usr/lib"
TARGET_MODEL_DIR="${TARGET_DIR}/usr/share/ocr/models"
TARGET_CONFIG_DIR="${TARGET_DIR}/etc/ocr"
TARGET_DATA_DIR="${TARGET_DIR}/data/ocr"
TARGET_FONT="${TARGET_DIR}/usr/share/fonts/noto/NotoSansCJK-Regular.ttc"
TARGET_BIN_DIR="${TARGET_DIR}/usr/bin"
CONFIG_SRC="${PROJECT_ROOT}/config"

# ---------------------- 辅助函数 ----------------------
log() {
    echo "[post_build] $*"
}

target_library_path() {
    pattern="$1"
    for directory in "${TARGET_DIR}/lib" "${TARGET_DIR}/usr/lib" \
                     "${TARGET_DIR}/lib64" "${TARGET_DIR}/usr/lib64"; do
        [ -d "${directory}" ] || continue
        found="$(find "${directory}" -type f -name "${pattern}" \
            -print -quit 2>/dev/null)"
        if [ -n "${found}" ]; then
            printf '%s\n' "${found}"
            return 0
        fi
    done
    return 1
}

verify_aarch64_elf() {
    elf_file="$1"
    description="$2"
    LC_ALL=C "${READELF}" -h "${elf_file}" 2>/dev/null | \
        grep -Eq 'Machine:[[:space:]]+AArch64' || {
        echo "错误: ${description} 不是 AArch64 ELF: ${elf_file}" >&2
        exit 1
    }
}

ensure_target_library() {
    description="$1"
    pattern="$2"
    fallback_dir="$3"
    installed="$(target_library_path "${pattern}" || true)"

    if [ -n "${installed}" ]; then
        verify_aarch64_elf "${installed}" "${description}"
        log "使用 vendor Buildroot 已安装的 ${description}: ${installed}"
        return
    fi

    [ -n "${fallback_dir}" ] || {
        echo "错误: target rootfs 缺少 ${description}，且未设置显式 fallback 目录" >&2
        exit 1
    }
    [ -d "${fallback_dir}" ] || {
        echo "错误: ${description} fallback 目录不存在: ${fallback_dir}" >&2
        exit 1
    }
    candidate="$(find "${fallback_dir}" \( -type f -o -type l \) \
        -name "${pattern}" -print -quit 2>/dev/null)"
    [ -n "${candidate}" ] || {
        echo "错误: ${fallback_dir} 中未找到 ${pattern}" >&2
        exit 1
    }
    verify_aarch64_elf "${candidate}" "${description} fallback"

    mkdir -p "${TARGET_LIB_DIR}"
    find "${fallback_dir}" \( -type f -o -type l \) -name "${pattern}" \
        -exec cp -av {} "${TARGET_LIB_DIR}/" \;
    installed="$(target_library_path "${pattern}" || true)"
    [ -n "${installed}" ] || {
        echo "错误: 无法安装 ${description}" >&2
        exit 1
    }
    verify_aarch64_elf "${installed}" "${description}"
    log "${description} fallback 安装完成"
}

# ---------------------- 步骤1: 拷贝 RKNN 运行时库 ----------------------
copy_rknn_libs() {
    ensure_target_library "librknnrt" 'librknnrt.so*' "${RKNN_LIB_DIR}"
}

# ---------------------- 步骤2: 拷贝 RGA 库 ----------------------
copy_rga_libs() {
    ensure_target_library "librga" 'librga.so*' "${RGA_LIB_DIR}"
}

copy_kernel_modules() {
    [ -s "${MODULE_ARCHIVE}" ] || {
        echo "错误: 缺少内核模块包 ${MODULE_ARCHIVE}" >&2
        exit 1
    }
    log "安装 vendor + OCR 内核模块"
    tar -xzf "${MODULE_ARCHIVE}" -C "${TARGET_DIR}"
}

# ---------------------- 步骤3: 拷贝模型文件 ----------------------
copy_models() {
    log "拷贝 RKNN 模型与词表"
    mkdir -p "${TARGET_MODEL_DIR}"

    if [ -d "${MODEL_DIR}" ]; then
        required="ppocrv4_det_int8.rknn ppocrv4_rec_fp16.rknn ppocr_keys_v1.txt
lite_transformer_encoder_fp16.rknn lite_transformer_decoder_fp16.rknn
translation_src_vocab.txt translation_tgt_vocab.txt translation_manifest.json"
        required="${required} model_artifacts.sha256"
        (cd "${MODEL_DIR}" && sha256sum -c model_artifacts.sha256) || {
            echo "错误: 模型/词表 SHA256 校验失败" >&2
            exit 1
        }
        for name in ${required}; do
            if [ ! -s "${MODEL_DIR}/${name}" ]; then
                echo "错误: 缺少模型部署产物 ${MODEL_DIR}/${name}" >&2
                exit 1
            fi
            cp -v "${MODEL_DIR}/${name}" "${TARGET_MODEL_DIR}/"
        done
        log "模型部署产物拷贝完成"
    else
        echo "错误: 模型目录不存在: ${MODEL_DIR}" >&2
        exit 1
    fi
}

# ---------------------- 步骤4: 拷贝配置文件 ----------------------
copy_configs() {
    log "拷贝配置文件"
    mkdir -p "${TARGET_CONFIG_DIR}"

    if [ -d "${CONFIG_SRC}" ]; then
        cp -v "${CONFIG_SRC}/ocr_translator.json" "${TARGET_CONFIG_DIR}/"
        log "运行时配置已拷贝（pipeline.json/sensors.json 仅为设计参考，不部署）"
    else
        log "警告: 配置源目录不存在: ${CONFIG_SRC}"
    fi
}

copy_font() {
    if [ -s "${TARGET_FONT}" ]; then
        log "目标 rootfs 已包含 Noto Sans CJK 字体"
        return
    fi
    if [ -n "${FONT_FILE:-}" ] && [ -s "${FONT_FILE}" ]; then
        mkdir -p "$(dirname "${TARGET_FONT}")"
        cp -v "${FONT_FILE}" "${TARGET_FONT}"
        return
    fi
    echo "错误: 缺少 ${TARGET_FONT}; 请让 vendor Buildroot 安装该字体或设置 FONT_FILE" >&2
    exit 1
}

# ---------------------- 步骤5: 创建数据目录 ----------------------
create_data_dir() {
    log "创建数据目录: ${TARGET_DATA_DIR}"
    mkdir -p "${TARGET_DATA_DIR}"
    # 设置目录权限 (应用可读写)
    chmod 755 "${TARGET_DATA_DIR}"
    if [ -f "${TARGET_DIR}/etc/init.d/S50ocr" ]; then
        chmod 755 "${TARGET_DIR}/etc/init.d/S50ocr"
    fi
}

# ---------------------- 步骤6: 设置动态库路径 ----------------------
setup_ldconfig() {
    log "配置动态库搜索路径"
    # 确保 /usr/lib 在动态库搜索路径中
    if [ -d "${TARGET_DIR}/etc" ]; then
        grep -qxF "/usr/lib" "${TARGET_DIR}/etc/ld.so.conf" 2>/dev/null || \
            echo "/usr/lib" >> "${TARGET_DIR}/etc/ld.so.conf"
        grep -qxF "/lib" "${TARGET_DIR}/etc/ld.so.conf" 2>/dev/null || \
            echo "/lib" >> "${TARGET_DIR}/etc/ld.so.conf"
    fi
}

# ---------------------- 步骤7: 拷贝应用可执行文件 ----------------------
copy_app() {
    log "拷贝应用可执行文件"
    mkdir -p "${TARGET_BIN_DIR}"

    if [ -f "${APP_BIN}" ]; then
        verify_aarch64_elf "${APP_BIN}" "ocr_translator"
        cp -v "${APP_BIN}" "${TARGET_BIN_DIR}/"
        chmod 755 "${TARGET_BIN_DIR}/ocr_translator"
        log "应用拷贝完成"
    else
        echo "错误: 应用可执行文件不存在: ${APP_BIN}; 请先运行 build_app.sh" >&2
        exit 1
    fi
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== post_build 开始 ==="
    log "目标 rootfs: ${TARGET_DIR}"

    if [ -z "$TARGET_DIR" ] || [ ! -d "$TARGET_DIR" ]; then
        echo "错误: 未传入有效的 target 目录" >&2
        exit 1
    fi
    command -v "${READELF}" >/dev/null 2>&1 || {
        echo "错误: 构建主机缺少 ${READELF}，无法校验目标 ELF 架构" >&2
        exit 1
    }

    copy_rknn_libs
    copy_rga_libs
    copy_kernel_modules
    copy_models
    copy_configs
    copy_font
    copy_app
    create_data_dir
    setup_ldconfig

    log "=== post_build 完成 ==="
    log "rootfs 内容概览:"
    ls -la "${TARGET_DIR}/usr/bin/ocr_translator" 2>/dev/null || true
    ls -la "${TARGET_MODEL_DIR}/"*.rknn 2>/dev/null || true
    ls -la "${TARGET_CONFIG_DIR}/"*.json 2>/dev/null || true
}

main "$@"
