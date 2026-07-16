#!/bin/bash
# Build the root filesystem with the Rockchip/LubanCat vendor Buildroot tree.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC_DIR="${BUILDROOT_SRC:-}"
EXTERNAL_DIR="${PROJECT_ROOT}/bsp/buildroot"
BUILD_DIR="${PROJECT_ROOT}/output/buildroot-build"
OUT_DIR="${PROJECT_ROOT}/output/rootfs"
DEFCONFIG_NAME="ocr_translator_defconfig"
APP_BIN="${APP_BIN:-${PROJECT_ROOT}/output/app/ocr_translator}"
MODEL_DIR="${MODEL_DIR:-${PROJECT_ROOT}/output/models}"
MODULE_ARCHIVE="${MODULE_ARCHIVE:-${PROJECT_ROOT}/output/kernel/modules.tar.gz}"
POST_BUILD_SCRIPT="${EXTERNAL_DIR}/board/rockchip/rk3576-lubancat3/post_build.sh"
READELF="${READELF:-readelf}"

log() { echo "[Rootfs $(date '+%H:%M:%S')] $*"; }
err() { echo "[Rootfs ERROR] $*" >&2; exit 1; }

check_source() {
    [ -n "${SRC_DIR}" ] || \
        err "请设置 BUILDROOT_SRC，指向包含 Rockchip RGA/RKNN 包的野火 SDK Buildroot"
    [ -f "${SRC_DIR}/Makefile" ] || err "无效 BUILDROOT_SRC: ${SRC_DIR}"
    [ -f "${EXTERNAL_DIR}/configs/${DEFCONFIG_NAME}" ] || \
        err "未找到 ${DEFCONFIG_NAME}"
    [ -f "${EXTERNAL_DIR}/external.desc" ] || \
        err "Buildroot external tree 不完整"
}

check_inputs() {
    local command
    for command in make sha256sum tar "${READELF}"; do
        command -v "${command}" >/dev/null 2>&1 || \
            err "构建主机缺少命令: ${command}"
    done
    [ -f "${POST_BUILD_SCRIPT}" ] || \
        err "缺少 Buildroot post-build 脚本: ${POST_BUILD_SCRIPT}"
    [ -x "${APP_BIN}" ] || \
        err "缺少应用产物: ${APP_BIN}; 请先完成 AArch64 应用构建"
    LC_ALL=C "${READELF}" -h "${APP_BIN}" 2>/dev/null | \
        grep -E 'Machine:[[:space:]]+AArch64' >/dev/null || \
        err "应用产物不是 AArch64 ELF: ${APP_BIN}"
    [ -s "${MODEL_DIR}/model_artifacts.sha256" ] || \
        err "缺少模型校验清单: ${MODEL_DIR}/model_artifacts.sha256"
    (cd "${MODEL_DIR}" && sha256sum -c model_artifacts.sha256 >/dev/null) || \
        err "模型产物 SHA256 预检失败: ${MODEL_DIR}"
    [ -s "${MODULE_ARCHIVE}" ] || \
        err "缺少内核模块包: ${MODULE_ARCHIVE}"
    tar -tzf "${MODULE_ARCHIVE}" >/dev/null || \
        err "内核模块包损坏: ${MODULE_ARCHIVE}"
    if [ -n "${FONT_FILE:-}" ] && [ ! -s "${FONT_FILE}" ]; then
        err "FONT_FILE 不存在或为空: ${FONT_FILE}"
    fi

    export APP_BIN MODEL_DIR MODULE_ARCHIVE READELF
}

configure() {
    mkdir -p "${BUILD_DIR}"
    chmod +x "${EXTERNAL_DIR}/board/rockchip/rk3576-lubancat3/post_build.sh"
    log "加载 ${DEFCONFIG_NAME}"
    make -C "${SRC_DIR}" O="${BUILD_DIR}" \
        BR2_EXTERNAL="${EXTERNAL_DIR}" "${DEFCONFIG_NAME}"
}

verify_config() {
    local config="${BUILD_DIR}/.config"
    local symbol
    local required=(
        BR2_aarch64
        BR2_cortex_a72
        BR2_TOOLCHAIN_EXTERNAL
        BR2_TOOLCHAIN_EXTERNAL_LINARO_AARCH64
        BR2_TOOLCHAIN_EXTERNAL_CXX
        BR2_INIT_SYSV
        BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV
        BR2_TARGET_ROOTFS_EXT2
        BR2_TARGET_ROOTFS_EXT2_4
        BR2_PACKAGE_LIBDRM
        BR2_PACKAGE_LIBRGA
        BR2_PACKAGE_RKNN_RUNTIME
        BR2_PACKAGE_FREETYPE
        BR2_PACKAGE_CJSON
        BR2_PACKAGE_JPEG
        BR2_PACKAGE_JPEG_TURBO
        BR2_PACKAGE_LIBIIO
        BR2_PACKAGE_LIBIIO_LOCAL_BACKEND
        BR2_PACKAGE_LIBIIO_TESTS
        BR2_PACKAGE_UTIL_LINUX_SCHEDUTILS
    )
    [ -s "${config}" ] || err "Buildroot did not generate ${config}"
    for symbol in "${required[@]}"; do
        grep -qx "${symbol}=y" "${config}" || \
            err "vendor Buildroot ignored required symbol ${symbol}"
    done
    grep -Eq '^BR2_ROOTFS_OVERLAY=".*rk3576-lubancat3/rootfs_overlay"$' \
        "${config}" || err "vendor Buildroot 未保留 OCR rootfs overlay"
    grep -Eq '^BR2_ROOTFS_POST_BUILD_SCRIPT=".*rk3576-lubancat3/post_build.sh"$' \
        "${config}" || err "vendor Buildroot 未保留 OCR post-build 脚本"
}

build() {
    mkdir -p "${OUT_DIR}"
    log "开始构建 vendor rootfs"
    make -C "${SRC_DIR}" O="${BUILD_DIR}" \
        BR2_EXTERNAL="${EXTERNAL_DIR}" -j"$(nproc)"
    local image="${BUILD_DIR}/images/rootfs.ext4"
    [ -f "${image}" ] || err "未找到 rootfs.ext4: ${image}"
    cp -v "${image}" "${OUT_DIR}/rootfs.ext4"
    log "构建完成: ${OUT_DIR}/rootfs.ext4"
}

main() {
    check_source
    check_inputs
    configure
    verify_config
    build
}

main "$@"
