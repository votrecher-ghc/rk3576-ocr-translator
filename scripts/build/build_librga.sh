#!/bin/bash
# 用目标工具链构建 vendor librga，并收集配套头文件和共享库供应用交叉链接。
# 生成的 toolchain 文件只用于本次构建，必须与应用使用的目标 ABI 相匹配。
# =====================================================================
# librga 交叉编译脚本 (Rockchip linux-rga / RK3576)
#
# 野火/Rockchip SDK 的 external/linux-rga 只提供源码, 没有 aarch64 预编译
# 库, 而 app/isp/rga_api.h 需要 im2d.h + librga.so。本脚本用 vendor SDK 的
# prebuilt 工具链把它编出来, 供 build_app.sh 的 OCR_RGA_* 变量使用。
#
# 依赖环境变量:
#   RGA_SRC       linux-rga 源码目录 (默认 ${RK_SDK_DIR}/external/linux-rga)
#   CROSS_COMPILE 交叉编译器前缀 (绝对路径前缀)
#   SYSROOT       目标 sysroot (见 make_sysroot.sh)
#
# 输出: output/librga/{lib/librga.so, include/im2d.h ...}
# =====================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

RK_SDK_DIR="${RK_SDK_DIR:-${HOME}/lubancat}"
RGA_SRC="${RGA_SRC:-${RK_SDK_DIR}/external/linux-rga}"
BUILD_DIR="${PROJECT_ROOT}/output/librga-build"
OUT_DIR="${PROJECT_ROOT}/output/librga"
JOBS=$(nproc)

log() { echo -e "[librga $(date '+%H:%M:%S')] $*"; }
err() { echo -e "[librga ERROR] $*" >&2; exit 1; }

check_prerequisites() {
    [ -f "${RGA_SRC}/CMakeLists.txt" ] || \
        err "未找到 linux-rga 源码: ${RGA_SRC}; 请设置 RGA_SRC 或 RK_SDK_DIR"
    [ -n "${CROSS_COMPILE:-}" ] || err "请设置 CROSS_COMPILE (绝对路径前缀)"
    command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || \
        err "未找到 ${CROSS_COMPILE}gcc"
    command -v "${CROSS_COMPILE}g++" >/dev/null 2>&1 || \
        err "未找到 ${CROSS_COMPILE}g++"
    [ -n "${SYSROOT:-}" ] && [ -d "${SYSROOT}" ] || \
        err "请设置 SYSROOT 指向目标 sysroot"
    SYSROOT="$(cd "${SYSROOT}" && pwd -P)"
    [ "${SYSROOT}" != "/" ] || err "拒绝使用宿主根目录作为 SYSROOT"
}

# SDK 自带的 toolchains/toolchain_linux.cmake 里是 vendor 打包机的硬编码路径,
# 这里按本机实际工具链与 sysroot 生成一份。
write_toolchain_file() {
    TOOLCHAIN_FILE="${BUILD_DIR}/toolchain-ocr.cmake"
    mkdir -p "${BUILD_DIR}"
    cat > "${TOOLCHAIN_FILE}" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER   "${CROSS_COMPILE}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_COMPILE}g++")
set(CMAKE_SYSROOT      "${SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
    log "工具链文件: ${TOOLCHAIN_FILE}"
}

configure() {
    log "CMake 配置 (源码: ${RGA_SRC})"
    rm -rf "${BUILD_DIR}/build"
    cmake -S "${RGA_SRC}" -B "${BUILD_DIR}/build" \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_BUILD_TARGET=cmake_linux \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="${OUT_DIR}"
}

build() {
    # 只编共享库目标, 不编 samples/im2d_api_demo
    log "编译 librga, 并发数: ${JOBS}"
    cmake --build "${BUILD_DIR}/build" --target rga -- -j"${JOBS}"
}

collect_outputs() {
    log "安装到 ${OUT_DIR}"
    rm -rf "${OUT_DIR}"
    mkdir -p "${OUT_DIR}/lib" "${OUT_DIR}/include"
    # 绕开 install(TARGETS rga-static ...) 对未构建静态库的依赖, 手动收集
    find "${BUILD_DIR}/build" -maxdepth 1 -name 'librga.so*' \
        -exec cp -av {} "${OUT_DIR}/lib/" \;
    [ -f "${OUT_DIR}/lib/librga.so" ] || err "未生成 librga.so"
    cp -av "${RGA_SRC}"/im2d_api/im2d*.h "${RGA_SRC}"/im2d_api/im2d.hpp \
           "${OUT_DIR}/include/" >/dev/null
    cp -av "${RGA_SRC}"/include/*.h "${OUT_DIR}/include/" >/dev/null

    log "=== 编译完成 ==="
    file "${OUT_DIR}/lib/librga.so"
    ls "${OUT_DIR}/include/" | tr '\n' ' '; echo
}

main() {
    log "=== librga 构建开始 ==="
    check_prerequisites
    write_toolchain_file
    configure
    build
    collect_outputs
    log "=== librga 构建结束 ==="
}

main "$@"
