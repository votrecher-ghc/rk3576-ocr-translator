#!/bin/bash
# =====================================================================
# U-Boot 编译脚本 (野火 LubanCat3 / RK3576)
# 功能: 克隆野火 U-Boot 源码 (lbc-develop 分支), 配置并交叉编译
# 输出: u-boot.bin, idbloader.img, u-boot.itb
# =====================================================================
set -e

# ---------------------- 路径与工具链变量 ----------------------
# 源码仓库地址 (野火官方)
UBOOT_REPO="https://gitee.com/lubancat/u-boot.git"
# 使用的分支
UBOOT_BRANCH="lbc-develop"
# 默认 defconfig
UBOOT_DEFCONFIG="lubancat3_rk3576_defconfig"

# 构建目录 (脚本所在目录的上级 build 输出)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC_DIR="${UBOOT_SRC:-${PROJECT_ROOT}/bsp/u-boot/src}"
OUT_DIR="${UBOOT_OUT:-${PROJECT_ROOT}/output/uboot}"

# 交叉编译工具链前缀
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
ARCH="${ARCH:-arm64}"

# 并发任务数
JOBS=$(nproc)

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[U-Boot $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[U-Boot ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 准备源码（默认不改动用户 SDK 分支） ----------------------
prepare_source() {
    if [ -f "${SRC_DIR}/Makefile" ]; then
        log "使用现有 U-Boot 源码: ${SRC_DIR}"
        cd "${SRC_DIR}"
    elif [ "${ALLOW_SOURCE_DOWNLOAD:-0}" = "1" ]; then
        log "克隆 U-Boot 源码: ${UBOOT_REPO} (分支: ${UBOOT_BRANCH})"
        git clone -b "${UBOOT_BRANCH}" "${UBOOT_REPO}" "${SRC_DIR}"
        cd "${SRC_DIR}"
    else
        err "U-Boot 源码不存在: ${SRC_DIR}; 请设置 UBOOT_SRC，或显式 ALLOW_SOURCE_DOWNLOAD=1"
    fi
}

# ---------------------- 步骤2: 配置 ----------------------
configure() {
    log "配置 defconfig: ${UBOOT_DEFCONFIG}"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${UBOOT_DEFCONFIG}"
}

# ---------------------- 步骤3: 编译 ----------------------
build() {
    mkdir -p "${OUT_DIR}"
    log "开始编译, 并发数: ${JOBS}"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}"

    # 拷贝输出产物
    log "拷贝输出产物到: ${OUT_DIR}"
    cp -v u-boot.bin "${OUT_DIR}/" 2>/dev/null || err "未找到 u-boot.bin"
    cp -v idbloader.img "${OUT_DIR}/" 2>/dev/null || err "未找到 idbloader.img"
    cp -v u-boot.itb "${OUT_DIR}/" 2>/dev/null || err "未找到 u-boot.itb"

    log "编译完成. 输出目录: ${OUT_DIR}"
    ls -lh "${OUT_DIR}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== U-Boot 构建开始 ==="
    prepare_source
    configure
    build
    log "=== U-Boot 构建结束 ==="
}

main "$@"
