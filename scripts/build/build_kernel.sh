#!/bin/bash
# =====================================================================
# Linux 内核编译脚本 (野火 LubanCat3 / RK3576)
# 功能: 克隆野火内核源码 (lbc-develop-6.1 分支), 配置并交叉编译
# 输出: Image, rk3576-lubancat3.dtb, *.dtbo, modules.tar.gz
# =====================================================================
set -euo pipefail

# ---------------------- 路径与工具链变量 ----------------------
# 内核源码仓库地址 (野火官方)
KERNEL_REPO="https://gitee.com/lubancat/linux.git"
# 使用的分支 (6.1 LTS)
KERNEL_BRANCH="lbc-develop-6.1"
# 默认 defconfig
KERNEL_DEFCONFIG="lubancat3_rk3576_defconfig"

# 构建目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC_DIR="${KERNEL_SRC:-${PROJECT_ROOT}/bsp/kernel/src}"
OUT_DIR="${PROJECT_ROOT}/output/kernel"

# 交叉编译工具链前缀
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
ARCH="${ARCH:-arm64}"

# 并发任务数
JOBS=$(nproc)

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Kernel $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[Kernel ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 准备源码（默认不改动用户 SDK 分支） ----------------------
prepare_source() {
    if [ -f "${SRC_DIR}/Makefile" ]; then
        log "使用现有内核源码: ${SRC_DIR}"
        cd "${SRC_DIR}"
    elif [ "${ALLOW_SOURCE_DOWNLOAD:-0}" = "1" ]; then
        log "克隆内核源码: ${KERNEL_REPO} (分支: ${KERNEL_BRANCH})"
        git clone -b "${KERNEL_BRANCH}" "${KERNEL_REPO}" "${SRC_DIR}"
        cd "${SRC_DIR}"
    else
        err "内核源码不存在: ${SRC_DIR}; 请设置 KERNEL_SRC，或显式 ALLOW_SOURCE_DOWNLOAD=1"
    fi
}

# ---------------------- 步骤2: 配置 defconfig ----------------------
configure() {
    log "配置 defconfig: ${KERNEL_DEFCONFIG}"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" "${KERNEL_DEFCONFIG}"
}

verify_ocr_dependencies() {
    local config="${SRC_DIR}/.config"
    local symbol
    local required=(
        CONFIG_MODULES CONFIG_GPIOLIB CONFIG_INPUT CONFIG_I2C CONFIG_SPI
        CONFIG_IIO CONFIG_IIO_BUFFER CONFIG_IIO_TRIGGER
        CONFIG_IIO_TRIGGERED_BUFFER CONFIG_IIO_KFIFO_BUF
        CONFIG_PWM CONFIG_PWM_SYSFS CONFIG_HWMON
    )

    [ -s "${config}" ] || err "内核配置不存在: ${config}"
    for symbol in "${required[@]}"; do
        grep -Eq "^${symbol}=(y|m)$" "${config}" ||
            err "vendor 内核缺少 OCR 外部模块依赖: ${symbol}=y/m"
    done
}

# ---------------------- 步骤3: 编译内核 Image ----------------------
build_image() {
    log "开始编译内核 Image, 并发数: ${JOBS}"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}" Image
}

# ---------------------- 步骤4: 编译 DTB 和 Overlay ----------------------
build_dtb() {
    local found_vendor_dtbo=0
    local dtbo
    log "编译 DTB 和 Overlay"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}" dtbs

    # 拷贝目标 DTB
    mkdir -p "${OUT_DIR}"
    DTB_FILE="arch/${ARCH}/boot/dts/rockchip/rk3576-lubancat3.dtb"
    if [ -f "${DTB_FILE}" ]; then
        cp -v "${DTB_FILE}" "${OUT_DIR}/"
    else
        err "未找到目标 DTB: ${DTB_FILE}"
    fi

    # 重建受管输出，避免旧 overlay 混入，同时保留本次 vendor + OCR 产物。
    rm -rf "${OUT_DIR}/dtbo"
    mkdir -p "${OUT_DIR}/dtbo"

    # 拷贝 Overlay (.dtbo)
    DTBO_DIR="arch/${ARCH}/boot/dts/rockchip/overlay"
    if [ -d "${DTBO_DIR}" ]; then
        for dtbo in "${DTBO_DIR}"/*.dtbo; do
            [ -f "${dtbo}" ] || continue
            cp -v "${dtbo}" "${OUT_DIR}/dtbo/"
            found_vendor_dtbo=1
        done
        [ "${found_vendor_dtbo}" -eq 1 ] || log "警告: 未找到 vendor .dtbo 文件"
    fi
}

# ---------------------- 步骤5: 编译模块并打包 ----------------------
build_modules() {
    log "编译内核模块"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" -j"${JOBS}" modules

    # 打包模块
    log "打包内核模块到 modules.tar.gz"
    rm -rf "${OUT_DIR}/modules"
    rm -f "${OUT_DIR}/modules.tar.gz"
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
        INSTALL_MOD_PATH="${OUT_DIR}/modules" \
        modules_install

}

build_ocr_modules() {
    local module_dir="${PROJECT_ROOT}/bsp/kernel/drivers"
    local module
    local ocr_configs=(
        CONFIG_OCR_GPIO_KEYS=m
        CONFIG_IIO_AP3216C=m
        CONFIG_IIO_ICM42688_SPI=m
        CONFIG_IIO_ADT7410=m
        CONFIG_PWM_FAN_OCR=m
    )
    local expected_modules=(
        gpio_keys_ocr
        iio_ap3216c
        iio_icm42688_spi
        iio_adt7410
        pwm_fan_ocr
    )
    log "编译项目自研 OCR 内核模块"
    make -C "${SRC_DIR}" M="${module_dir}" ARCH="${ARCH}" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        "${ocr_configs[@]}" -j"${JOBS}" modules

    # External-module Kbuild variables are command-line-only; pass the exact
    # same set to modules_install so it cannot see an empty obj-$(CONFIG_*) set.
    make -C "${SRC_DIR}" M="${module_dir}" ARCH="${ARCH}" \
        CROSS_COMPILE="${CROSS_COMPILE}" \
        "${ocr_configs[@]}" \
        INSTALL_MOD_PATH="${OUT_DIR}/modules" modules_install

    for module in "${expected_modules[@]}"; do
        find "${OUT_DIR}/modules/lib/modules" -type f \
            -name "${module}.ko*" -print -quit 2>/dev/null | grep -q . || \
            err "OCR 模块未安装到 modules 包: ${module}.ko"
    done

    tar -czf "${OUT_DIR}/modules.tar.gz" -C "${OUT_DIR}/modules" .
    log "模块打包完成: ${OUT_DIR}/modules.tar.gz"
}

build_ocr_overlays() {
    local overlay_src="${PROJECT_ROOT}/bsp/kernel/arch/arm64/boot/dts/rockchip/overlays"
    local overlay_out="${OUT_DIR}/dtbo"
    local dtc_bin="${SRC_DIR}/scripts/dtc/dtc"
    [ -x "${dtc_bin}" ] || dtc_bin="$(command -v dtc || true)"
    [ -n "${dtc_bin}" ] || err "未找到 dtc"
    command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || \
        err "未找到 ${CROSS_COMPILE}gcc"
    mkdir -p "${overlay_out}"
    local source preprocessed output
    for source in "${overlay_src}"/*.dtso; do
        [ -f "${source}" ] || err "未找到 OCR overlay 源文件: ${overlay_src}"
        if [ "$(basename "${source}")" = "imx415-csi2.dtso" ] &&
           [ "${BUILD_UNVERIFIED_CAMERA_OVERLAY:-0}" != "1" ]; then
            log "  跳过未按 vendor media graph 核对的 imx415-csi2.dtso"
            continue
        fi
        preprocessed="$(mktemp)"
        # Prefix project overlays so a generic basename can never overwrite a
        # vendor DTBO collected into the same staging directory.
        output="${overlay_out}/ocr-$(basename "${source}" .dtso).dtbo"
        if ! "${CROSS_COMPILE}gcc" -E -nostdinc -undef -D__DTS__ \
            -I "${SRC_DIR}/include" \
            -I "${SRC_DIR}/arch/${ARCH}/boot/dts" \
            -I "${SRC_DIR}/arch/${ARCH}/boot/dts/rockchip" \
            -x assembler-with-cpp "${source}" > "${preprocessed}"; then
            rm -f "${preprocessed}"
            err "预处理 overlay 失败: ${source}"
        fi
        if ! "${dtc_bin}" -@ -I dts -O dtb -o "${output}" "${preprocessed}"; then
            rm -f "${preprocessed}"
            err "编译 overlay 失败: ${source}"
        fi
        rm -f "${preprocessed}"
        log "  已生成: ${output}"
    done
}

# ---------------------- 步骤6: 收集输出产物 ----------------------
collect_outputs() {
    log "拷贝内核 Image"
    cp -v "arch/${ARCH}/boot/Image" "${OUT_DIR}/" 2>/dev/null || err "未找到内核 Image"

    log "=== 编译完成. 输出目录: ${OUT_DIR} ==="
    ls -lh "${OUT_DIR}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== 内核构建开始 ==="
    prepare_source
    configure
    verify_ocr_dependencies
    build_image
    build_dtb
    build_modules
    build_ocr_modules
    build_ocr_overlays
    collect_outputs
    log "=== 内核构建结束 ==="
}

main "$@"
