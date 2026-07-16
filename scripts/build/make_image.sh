#!/bin/bash
# =====================================================================
# 固件打包脚本 (野火 LubanCat3 / RK3576)
# 功能: 打包 boot.img + rootfs.img + resource (dtbo) 为完整固件
# 工具: mkimage；完整 update.img 需 vendor SDK 打包包装器
# 输出: 分区镜像集合，可选 update.img
# =====================================================================
set -euo pipefail

# ---------------------- 路径与工具链变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT_DIR="${PROJECT_ROOT}/output"

# 各组件输出目录
UBOOT_OUT="${OUT_DIR}/uboot"
KERNEL_OUT="${OUT_DIR}/kernel"
ROOTFS_OUT="${OUT_DIR}/rootfs"
FW_OUT="${OUT_DIR}/firmware"

# 所需文件
IDBLOADER_IMG="${UBOOT_OUT}/idbloader.img"
UBOOT_ITB="${UBOOT_OUT}/u-boot.itb"
KERNEL_IMAGE="${KERNEL_OUT}/Image"
KERNEL_DTB="${KERNEL_OUT}/rk3576-lubancat3.dtb"
DTBO_DIR="${KERNEL_OUT}/dtbo"
ROOTFS_IMG="${ROOTFS_OUT}/rootfs.ext4"

# 可选 vendor 打包包装器，接口: wrapper <firmware-dir> <output-update.img>
RK_PACK_SCRIPT="${RK_PACK_SCRIPT:-}"
# mkimage 路径 (备选打包方式)
MKIMAGE="${MKIMAGE:-mkimage}"
FDTOVERLAY="${FDTOVERLAY:-fdtoverlay}"
BOOT_DTB="${KERNEL_DTB}"
# Space-separated DTBO basenames to apply, for example:
# OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-pwm-fan.dtbo".
# The safe default is an unmodified vendor base DTB because the checked-in
# OCR overlays still contain board-specific pin and supply placeholders.
OCR_DTBO_LIST="${OCR_DTBO_LIST:-}"
# Exactly one activation path is allowed for selected overlays:
#   merged          - fold them into the FIT's DTB (default)
#   vendor-resource - leave base DTB untouched and require RK_PACK_SCRIPT to
#                     package the selected DTBOs into the vendor firmware
DT_OVERLAY_MODE="${DT_OVERLAY_MODE:-merged}"
SELECTED_DTBO=()

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Image $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[Image ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 检查输入文件 ----------------------
check_inputs() {
    log "检查输入文件"
    local missing=0
    for f in "${IDBLOADER_IMG}" "${UBOOT_ITB}" \
             "${KERNEL_IMAGE}" "${KERNEL_DTB}" "${ROOTFS_IMG}"; do
        if [ ! -f "$f" ]; then
            log "  缺失: $f"
            missing=$((missing + 1))
        else
            log "  就绪: $(basename "$f")"
        fi
    done
    [ "${missing}" -gt 0 ] && err "缺少 ${missing} 个必需文件, 请先运行对应构建脚本"
}

prepare_output() {
    mkdir -p "${FW_OUT}"
    # Remove only artifacts managed here so a stale vendor update.img cannot
    # be mistaken for the image produced by the current build.
    rm -f "${FW_OUT}/boot.its" "${FW_OUT}/boot.img" \
        "${FW_OUT}/idbloader.img" "${FW_OUT}/u-boot.itb" \
        "${FW_OUT}/rootfs.img" "${FW_OUT}/update.img" \
        "${FW_OUT}/rk3576-lubancat3-ocr.dtb"
    rm -rf "${FW_OUT}/dtbo"
}

select_overlays() {
    SELECTED_DTBO=()
    if [ -z "${OCR_DTBO_LIST}" ]; then
        log "未设置 OCR_DTBO_LIST；启动 DTB 不自动应用任何 overlay"
        return
    fi

    case "${DT_OVERLAY_MODE}" in
        merged|vendor-resource) ;;
        *) err "DT_OVERLAY_MODE 只接受 merged 或 vendor-resource" ;;
    esac

    local names=()
    local name path
    read -r -a names <<< "${OCR_DTBO_LIST}"
    for name in "${names[@]}"; do
        case "${name}" in
            *.dtbo) ;;
            *) err "OCR_DTBO_LIST 只接受 .dtbo 文件名: ${name}" ;;
        esac
        case "${name}" in
            */*|*\\*) err "OCR_DTBO_LIST 只接受文件名，不能包含路径: ${name}" ;;
        esac
        path="${DTBO_DIR}/${name}"
        [ -f "${path}" ] || err "未找到指定 overlay: ${path}"
        SELECTED_DTBO+=("${path}")
    done
    [ "${#SELECTED_DTBO[@]}" -gt 0 ] || err "OCR_DTBO_LIST 未包含有效 overlay"
    if [ "${DT_OVERLAY_MODE}" = "vendor-resource" ]; then
        [ -n "${RK_PACK_SCRIPT}" ] || \
            err "vendor-resource 模式要求设置 RK_PACK_SCRIPT，以实际打包并启用所选 overlay"
        [ -x "${RK_PACK_SCRIPT}" ] || \
            err "RK_PACK_SCRIPT 不可执行: ${RK_PACK_SCRIPT}"
    fi
    log "已显式选择 ${#SELECTED_DTBO[@]} 个 overlay"
}

prepare_boot_dtb() {
    BOOT_DTB="${KERNEL_DTB}"
    if [ "${#SELECTED_DTBO[@]}" -gt 0 ] &&
       [ "${DT_OVERLAY_MODE}" = "merged" ]; then
        command -v "${FDTOVERLAY}" >/dev/null 2>&1 || \
            err "已选择 DTBO，但未找到 fdtoverlay"
        BOOT_DTB="${FW_OUT}/rk3576-lubancat3-ocr.dtb"
        "${FDTOVERLAY}" -i "${KERNEL_DTB}" -o "${BOOT_DTB}" \
            "${SELECTED_DTBO[@]}" || \
            err "合并设备树 overlay 失败，请核对 vendor DTB 标签与板卡硬件连接"
        log "已将 ${#SELECTED_DTBO[@]} 个显式选择的 overlay 合并到启动 DTB"
    elif [ "${#SELECTED_DTBO[@]}" -gt 0 ]; then
        log "overlay 将由 RK_PACK_SCRIPT 的 vendor resource 路径启用；启动 FIT 保留 base DTB"
    fi
}

# ---------------------- 步骤2: 生成 boot.img ----------------------
make_boot_img() {
    mkdir -p "${FW_OUT}"
    prepare_boot_dtb
    log "生成 boot.img (内核 Image + DTB)"

    # 使用 mkimage 创建 FIT 格式 boot image
    cat > "${FW_OUT}/boot.its" <<EOF
/dts-v1/;
/ {
    description = "LubanCat3 RK3576 boot image";
    images {
        kernel {
            description = "Linux kernel";
            data = /incbin/("${KERNEL_IMAGE}");
            type = "kernel";
            arch = "arm64";
            os = "linux";
            compression = "none";
            load = <0x00280000>;
            entry = <0x00280000>;
        };
        fdt {
            description = "Flattened Device Tree";
            data = /incbin/("${BOOT_DTB}");
            type = "flat_dt";
            arch = "arm64";
            compression = "none";
        };
    };
    configurations {
        default = "conf";
        conf {
            description = "Default configuration";
            kernel = "kernel";
            fdt = "fdt";
        };
    };
};
EOF

    "${MKIMAGE}" -f "${FW_OUT}/boot.its" "${FW_OUT}/boot.img" \
        || err "生成 boot.img 失败"
    log "boot.img 生成完成"
}

# ---------------------- 步骤3: 打包 resource (dtbo) ----------------------
make_resource() {
    log "打包 resource (dtbo overlay)"
    if [ "${#SELECTED_DTBO[@]}" -gt 0 ] &&
       [ "${DT_OVERLAY_MODE}" = "vendor-resource" ]; then
        # Rockchip resource.img 格式依赖 vendor 工具，不用 U-Boot multi
        # image 冒充；将显式选择的 dtbo 交给已验证可执行的 vendor pack wrapper。
        mkdir -p "${FW_OUT}/dtbo"
        local dtbo
        for dtbo in "${SELECTED_DTBO[@]}"; do
            cp -v "${dtbo}" "${FW_OUT}/dtbo/"
        done
        log "已收集显式选择的 dtbo"
    elif [ "${#SELECTED_DTBO[@]}" -eq 0 ]; then
        log "未选择 dtbo，跳过 resource 打包"
    else
        log "overlay 已合并进启动 DTB，不再复制到 vendor resource，避免重复应用"
    fi
}

# ---------------------- 步骤4: 生成完整 update.img ----------------------
make_update_img() {
    log "生成完整固件 update.img"

    # 拷贝各组件到固件输出目录
    cp -v "${IDBLOADER_IMG}" "${FW_OUT}/"
    cp -v "${UBOOT_ITB}" "${FW_OUT}/"
    cp -v "${ROOTFS_IMG}" "${FW_OUT}/rootfs.img"

    if [ -n "${RK_PACK_SCRIPT}" ]; then
        [ -x "${RK_PACK_SCRIPT}" ] || err "RK_PACK_SCRIPT 不可执行: ${RK_PACK_SCRIPT}"
        log "调用 vendor 固件打包包装器"
        "${RK_PACK_SCRIPT}" "${FW_OUT}" "${FW_OUT}/update.img"
        [ -s "${FW_OUT}/update.img" ] || err "vendor 包装器未生成 update.img"
    else
        log "未设置 RK_PACK_SCRIPT；已生成分区镜像集合，不生成伪 update.img"
        log "请使用 vendor SDK 的 parameter.txt/MiniLoader 与打包工具生成完整固件"
    fi
}

# ---------------------- 步骤5: 输出汇总 ----------------------
summary() {
    log "=== 固件打包完成. 输出目录: ${FW_OUT} ==="
    ls -lh "${FW_OUT}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== 固件打包开始 ==="
    check_inputs
    prepare_output
    select_overlays
    make_boot_img
    make_resource
    make_update_img
    summary
    log "=== 固件打包结束 ==="
}

main "$@"
