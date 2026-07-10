#!/bin/sh
# =====================================================================
# post_build.sh - Buildroot 构建后处理脚本
# 功能: 在 rootfs 构建完成后, 拷贝运行时依赖到目标文件系统
# 由 Buildroot 在 make 完成后自动调用
# 参数: $1 = target 目录 (即 rootfs 根目录)
# =====================================================================
set -e

# ---------------------- 路径变量 ----------------------
# Buildroot 传入的目标 rootfs 目录
TARGET_DIR="$1"

# 项目根目录 (本脚本所在路径上溯)
BOARD_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${BOARD_DIR}/../../.." && pwd)"

# 第三方库源目录 (RKNN 运行时, RGA 库等)
RKNN_LIB_DIR="${RKNN_LIB_DIR:-${PROJECT_ROOT}/bsp/libs/rknn}"
RGA_LIB_DIR="${RGA_LIB_DIR:-${PROJECT_ROOT}/bsp/libs/rga}"

# 模型文件目录
MODEL_DIR="${MODEL_DIR:-${PROJECT_ROOT}/output/models}"

# 目标安装路径
TARGET_LIB_DIR="${TARGET_DIR}/usr/lib"
TARGET_MODEL_DIR="${TARGET_DIR}/usr/share/ocr/models"
TARGET_CONFIG_DIR="${TARGET_DIR}/etc/ocr"
TARGET_DATA_DIR="${TARGET_DIR}/data/ocr"

# ---------------------- 辅助函数 ----------------------
log() {
    echo "[post_build] $*"
}

# ---------------------- 步骤1: 拷贝 RKNN 运行时库 ----------------------
copy_rknn_libs() {
    log "拷贝 RKNN 运行时库"
    if [ -d "${RKNN_LIB_DIR}" ]; then
        mkdir -p "${TARGET_LIB_DIR}"
        # 拷贝共享库 (.so)
        find "${RKNN_LIB_DIR}" -name "*.so*" -exec cp -v {} "${TARGET_LIB_DIR}/" \;
        log "RKNN 库拷贝完成"
    else
        log "警告: RKNN 库目录不存在: ${RKNN_LIB_DIR}"
        log "请从 Rockchip SDK 获取 librknnrt.so 并放置到该目录"
    fi
}

# ---------------------- 步骤2: 拷贝 RGA 库 ----------------------
copy_rga_libs() {
    log "拷贝 RGA 库"
    if [ -d "${RGA_LIB_DIR}" ]; then
        mkdir -p "${TARGET_LIB_DIR}"
        find "${RGA_LIB_DIR}" -name "*.so*" -exec cp -v {} "${TARGET_LIB_DIR}/" \;
        log "RGA 库拷贝完成"
    else
        log "警告: RGA 库目录不存在: ${RGA_LIB_DIR}"
        log "请从 Rockchip SDK 获取 librga.so 并放置到该目录"
    fi
}

# ---------------------- 步骤3: 拷贝模型文件 ----------------------
copy_models() {
    log "拷贝 RKNN 模型文件"
    mkdir -p "${TARGET_MODEL_DIR}"

    if [ -d "${MODEL_DIR}" ]; then
        local found=0
        for model in "${MODEL_DIR}"/*.rknn; do
            if [ -f "$model" ]; then
                cp -v "$model" "${TARGET_MODEL_DIR}/"
                found=$((found + 1))
            fi
        done
        if [ "$found" -eq 0 ]; then
            log "警告: 未找到 .rknn 模型文件, 请先运行 build_models.sh"
        else
            log "共拷贝 ${found} 个模型文件"
        fi
    else
        log "警告: 模型目录不存在: ${MODEL_DIR}"
    fi
}

# ---------------------- 步骤4: 拷贝配置文件 ----------------------
copy_configs() {
    log "拷贝配置文件"
    mkdir -p "${TARGET_CONFIG_DIR}"

    local CONFIG_SRC="${PROJECT_ROOT}/config"
    if [ -d "${CONFIG_SRC}" ]; then
        cp -v "${CONFIG_SRC}/ocr_translator.json" "${TARGET_CONFIG_DIR}/" 2>/dev/null || true
        cp -v "${CONFIG_SRC}/pipeline.json" "${TARGET_CONFIG_DIR}/" 2>/dev/null || true
        cp -v "${CONFIG_SRC}/sensors.json" "${TARGET_CONFIG_DIR}/" 2>/dev/null || true
        log "配置文件拷贝完成"
    else
        log "警告: 配置源目录不存在: ${CONFIG_SRC}"
    fi
}

# ---------------------- 步骤5: 创建数据目录 ----------------------
create_data_dir() {
    log "创建数据目录: ${TARGET_DATA_DIR}"
    mkdir -p "${TARGET_DATA_DIR}"
    # 设置目录权限 (应用可读写)
    chmod 755 "${TARGET_DATA_DIR}"
}

# ---------------------- 步骤6: 设置动态库路径 ----------------------
setup_ldconfig() {
    log "配置动态库搜索路径"
    # 确保 /usr/lib 在动态库搜索路径中
    if [ -d "${TARGET_DIR}/etc" ]; then
        echo "/usr/lib" >> "${TARGET_DIR}/etc/ld.so.conf" 2>/dev/null || true
        echo "/lib" >> "${TARGET_DIR}/etc/ld.so.conf" 2>/dev/null || true
    fi
}

# ---------------------- 步骤7: 拷贝应用可执行文件 ----------------------
copy_app() {
    local APP_BIN="${PROJECT_ROOT}/output/app/ocr_translator"
    local TARGET_BIN_DIR="${TARGET_DIR}/usr/bin"

    log "拷贝应用可执行文件"
    mkdir -p "${TARGET_BIN_DIR}"

    if [ -f "${APP_BIN}" ]; then
        cp -v "${APP_BIN}" "${TARGET_BIN_DIR}/"
        chmod 755 "${TARGET_BIN_DIR}/ocr_translator"
        log "应用拷贝完成"
    else
        log "警告: 应用可执行文件不存在: ${APP_BIN}"
        log "请先运行 build_app.sh"
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

    copy_rknn_libs
    copy_rga_libs
    copy_models
    copy_configs
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
