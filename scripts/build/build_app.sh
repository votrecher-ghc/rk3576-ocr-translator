#!/bin/bash
# =====================================================================
# 应用程序交叉编译脚本 (CMake, 野火 LubanCat3 / RK3576)
# 功能: 使用 CMake 工具链文件进行交叉编译
# 输出: ocr_translator 可执行文件
# =====================================================================
set -euo pipefail

# ---------------------- 路径与工具链变量 ----------------------
# 构建目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
APP_SRC="${PROJECT_ROOT}/app"
BUILD_DIR="${APP_SRC}/build"
OUT_DIR="${PROJECT_ROOT}/output/app"
APP_BINARY="${BUILD_DIR}/bin/ocr_translator"

# CMake 工具链文件路径
CMAKE_TOOLCHAIN_FILE="${APP_SRC}/cmake/aarch64-linux.cmake"

# 并发任务数
JOBS=$(nproc)

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[App $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[App ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 检查源码与工具链 ----------------------
check_prerequisites() {
    local compiler="${CROSS_COMPILE:-aarch64-none-linux-gnu-}gcc"
    local compiler_target
    local detected_sysroot

    if [ ! -d "${APP_SRC}" ]; then
        err "应用源码目录不存在: ${APP_SRC}"
    fi
    if [ ! -f "${CMAKE_TOOLCHAIN_FILE}" ]; then
        err "CMake 工具链文件不存在: ${CMAKE_TOOLCHAIN_FILE}"
    fi
    command -v cmake >/dev/null 2>&1 || err "未找到 cmake 命令"
    command -v "${compiler}" >/dev/null 2>&1 || \
        err "未找到 AArch64 交叉编译器: ${compiler}"

    compiler_target="$("${compiler}" -dumpmachine)"
    case "${compiler_target}" in
        aarch64*) ;;
        *) err "交叉编译器目标不是 AArch64: ${compiler_target}" ;;
    esac

    if [ -z "${SYSROOT:-}" ]; then
        detected_sysroot="$("${compiler}" -print-sysroot)"
        [ -n "${detected_sysroot}" ] || \
            err "交叉编译器未提供 sysroot；请设置 SYSROOT 指向 vendor SDK sysroot"
        SYSROOT="${detected_sysroot}"
    fi
    [ "${SYSROOT}" != "/" ] || \
        err "拒绝使用宿主根目录作为 SYSROOT；请指定 vendor SDK sysroot"
    [ -d "${SYSROOT}" ] || err "SYSROOT 目录不存在: ${SYSROOT}"
    SYSROOT="$(cd "${SYSROOT}" && pwd -P)"
    [ -d "${SYSROOT}/usr/include" ] || \
        err "SYSROOT 缺少目标头文件目录: ${SYSROOT}/usr/include"

    CROSS_COMPILE="${CROSS_COMPILE:-aarch64-none-linux-gnu-}"
    export CROSS_COMPILE SYSROOT
    log "目标工具链: ${compiler_target}; sysroot: ${SYSROOT}"
}

# ---------------------- 步骤2: CMake 配置 ----------------------
configure() {
    local cmake_args=(
        -DCMAKE_TOOLCHAIN_FILE="${CMAKE_TOOLCHAIN_FILE}"
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_TESTING=OFF
    )

    [ -z "${OCR_RGA_INCLUDE_DIR:-}" ] || \
        cmake_args+=("-DOCR_RGA_INCLUDE_DIR=${OCR_RGA_INCLUDE_DIR}")
    [ -z "${OCR_RGA_LIBRARY:-}" ] || \
        cmake_args+=("-DOCR_RGA_LIBRARY=${OCR_RGA_LIBRARY}")
    [ -z "${OCR_RKNN_INCLUDE_DIR:-}" ] || \
        cmake_args+=("-DOCR_RKNN_INCLUDE_DIR=${OCR_RKNN_INCLUDE_DIR}")
    [ -z "${OCR_RKNN_LIBRARY:-}" ] || \
        cmake_args+=("-DOCR_RKNN_LIBRARY=${OCR_RKNN_LIBRARY}")

    log "清理旧构建目录: ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
    mkdir -p "${BUILD_DIR}"
    cd "${BUILD_DIR}"

    log "CMake 配置 (工具链: ${CMAKE_TOOLCHAIN_FILE})"
    cmake "${APP_SRC}" "${cmake_args[@]}"
}

# ---------------------- 步骤3: 编译 ----------------------
build() {
    log "开始编译, 并发数: ${JOBS}"
    cmake --build "${BUILD_DIR}" -- -j"${JOBS}"

    # 校验输出可执行文件
    if [ ! -f "${APP_BINARY}" ]; then
        err "未找到输出可执行文件: ${APP_BINARY}"
    fi
    log "编译成功: ${APP_BINARY}"
}

# ---------------------- 步骤4: 收集输出产物 ----------------------
collect_outputs() {
    mkdir -p "${OUT_DIR}"
    cp -v "${APP_BINARY}" "${OUT_DIR}/"

    log "=== 编译完成. 输出目录: ${OUT_DIR} ==="
    ls -lh "${OUT_DIR}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== 应用程序构建开始 ==="
    check_prerequisites
    configure
    build
    collect_outputs
    log "=== 应用程序构建结束 ==="
}

main "$@"
