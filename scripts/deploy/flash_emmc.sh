#!/bin/bash
# =====================================================================
# eMMC 烧录脚本 (野火 LubanCat3 / RK3576)
# 功能: 将编译好的固件烧录到开发板 eMMC
# 方式: 优先使用 Rockchip upgrade_tool (USB 烧录), 备选 dd 本地烧录
# =====================================================================
set -e

# ---------------------- 路径与设备变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FW_DIR="${PROJECT_ROOT}/output/firmware"

# 各分区镜像
IDBLOADER_IMG="${FW_DIR}/idbloader.img"
UBOOT_ITB="${FW_DIR}/u-boot.itb"
BOOT_IMG="${FW_DIR}/boot.img"
ROOTFS_IMG="${FW_DIR}/rootfs.img"
UPDATE_IMG="${FW_DIR}/update.img"

# Rockchip 烧录工具
UPGRADE_TOOL="${UPGRADE_TOOL:-upgrade_tool}"

# 本地烧录设备 (使用 dd 方式时, 如通过 USB 读卡器烧录 SD/eMMC)
# 警告: 请确认设备路径, 误烧可能导致数据丢失
LOCAL_DEVICE="${LOCAL_DEVICE:-}"

# 分区偏移 (单位: 扇区, 512B)
OFFSET_IDBLOADER="${OFFSET_IDBLOADER:-}"
OFFSET_UBOOT="${OFFSET_UBOOT:-}"
OFFSET_BOOT="${OFFSET_BOOT:-}"
OFFSET_ROOTFS="${OFFSET_ROOTFS:-}"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Flash $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[Flash ERROR] $*" >&2
    exit 1
}

parse_offset() {
    local raw="$1"
    if [[ "${raw}" =~ ^0[xX][0-9a-fA-F]+$ ]]; then
        printf '%d\n' "$((16#${raw:2}))"
    elif [[ "${raw}" =~ ^[0-9]+$ ]]; then
        printf '%d\n' "$((10#${raw}))"
    else
        return 1
    fi
}

image_sectors() {
    local bytes
    bytes=$(stat -c %s "$1") || return 1
    printf '%d\n' "$(( (bytes + 511) / 512 ))"
}

validate_offsets() {
    OFFSET_IDBLOADER_DEC=$(parse_offset "${OFFSET_IDBLOADER}") || err "无效 OFFSET_IDBLOADER"
    OFFSET_UBOOT_DEC=$(parse_offset "${OFFSET_UBOOT}") || err "无效 OFFSET_UBOOT"
    OFFSET_BOOT_DEC=$(parse_offset "${OFFSET_BOOT}") || err "无效 OFFSET_BOOT"
    OFFSET_ROOTFS_DEC=$(parse_offset "${OFFSET_ROOTFS}") || err "无效 OFFSET_ROOTFS"

    local idb_sectors uboot_sectors boot_sectors
    idb_sectors=$(image_sectors "${IDBLOADER_IMG}") || err "无法读取 idbloader 大小"
    uboot_sectors=$(image_sectors "${UBOOT_ITB}") || err "无法读取 U-Boot 大小"
    boot_sectors=$(image_sectors "${BOOT_IMG}") || err "无法读取 boot.img 大小"
    [ $((OFFSET_IDBLOADER_DEC + idb_sectors)) -le "${OFFSET_UBOOT_DEC}" ] || \
        err "idbloader 与 U-Boot 烧录范围重叠"
    [ $((OFFSET_UBOOT_DEC + uboot_sectors)) -le "${OFFSET_BOOT_DEC}" ] || \
        err "U-Boot 与 boot.img 烧录范围重叠"
    [ $((OFFSET_BOOT_DEC + boot_sectors)) -le "${OFFSET_ROOTFS_DEC}" ] || \
        err "boot.img 与 rootfs 烧录范围重叠"
}

# ---------------------- 步骤1: 检查输入文件 ----------------------
check_inputs() {
    log "检查烧录文件"
    local required=("${IDBLOADER_IMG}" "${UBOOT_ITB}" "${BOOT_IMG}" "${ROOTFS_IMG}")
    for f in "${required[@]}"; do
        [ -f "$f" ] || err "缺少文件: $f"
        log "  就绪: $(basename "$f")"
    done
}

# ---------------------- 步骤2: USB 烧录 (upgrade_tool) ----------------------
flash_via_upgrade_tool() {
    if ! command -v "${UPGRADE_TOOL}" >/dev/null 2>&1; then
        log "未找到 ${UPGRADE_TOOL}, 跳过 USB 烧录方式"
        return 1
    fi

    log "检测开发板是否进入 MaskROM/Loader 模式"
    if ! "${UPGRADE_TOOL}" ld 2>/dev/null | grep -q "LubanCat\|RK3576\|DEV"; then
        err "未检测到设备, 请将开发板进入 MaskROM 模式 (按住 MASKROM 键后上电)"
    fi

    log "开始 USB 烧录"

    # 优先使用完整 update.img (若存在)
    if [ -f "${UPDATE_IMG}" ]; then
        log "烧录完整固件: update.img"
        "${UPGRADE_TOOL}" uf "${UPDATE_IMG}" \
            || err "update.img 烧录失败"
    else
        [ -n "${OFFSET_IDBLOADER}" ] && [ -n "${OFFSET_UBOOT}" ] && \
        [ -n "${OFFSET_BOOT}" ] && [ -n "${OFFSET_ROOTFS}" ] || \
            err "分区烧录必须按板卡 parameter.txt 显式设置全部 OFFSET_* 扇区"
        validate_offsets
        # 分区烧录
        log "烧录 idbloader (偏移: ${OFFSET_IDBLOADER})"
        "${UPGRADE_TOOL}" wl "${OFFSET_IDBLOADER_DEC}" "${IDBLOADER_IMG}" \
            || err "idbloader 烧录失败"

        log "烧录 u-boot.itb (偏移: ${OFFSET_UBOOT})"
        "${UPGRADE_TOOL}" wl "${OFFSET_UBOOT_DEC}" "${UBOOT_ITB}" \
            || err "u-boot 烧录失败"

        log "烧录 boot.img (偏移: ${OFFSET_BOOT})"
        "${UPGRADE_TOOL}" wl "${OFFSET_BOOT_DEC}" "${BOOT_IMG}" \
            || err "boot.img 烧录失败"

        log "烧录 rootfs.img (偏移: ${OFFSET_ROOTFS})"
        "${UPGRADE_TOOL}" wl "${OFFSET_ROOTFS_DEC}" "${ROOTFS_IMG}" \
            || err "rootfs 烧录失败"
    fi

    log "USB 烧录完成"
    return 0
}

# ---------------------- 步骤3: 本地 dd 烧录 (备选) ----------------------
flash_via_dd() {
    [ -n "${LOCAL_DEVICE}" ] || err "dd 模式必须显式设置 LOCAL_DEVICE"
    [ -b "${LOCAL_DEVICE}" ] || err "LOCAL_DEVICE 不是块设备: ${LOCAL_DEVICE}"
    [ -n "${OFFSET_IDBLOADER}" ] && [ -n "${OFFSET_UBOOT}" ] && \
    [ -n "${OFFSET_BOOT}" ] && [ -n "${OFFSET_ROOTFS}" ] || \
        err "dd 模式必须按板卡 parameter.txt 显式设置全部 OFFSET_* 扇区"
    validate_offsets
    local rootfs_sectors device_sectors
    rootfs_sectors=$(image_sectors "${ROOTFS_IMG}") || err "无法读取 rootfs 大小"
    device_sectors=$(blockdev --getsz "${LOCAL_DEVICE}") || err "无法读取目标设备容量"
    [ $((OFFSET_ROOTFS_DEC + rootfs_sectors)) -le "${device_sectors}" ] || \
        err "rootfs 镜像超出目标设备容量"
    log "警告: 使用 dd 本地烧录方式"
    log "目标设备: ${LOCAL_DEVICE}"
    log "请确认设备路径正确, 误烧将导致数据丢失!"
    read -r -p "输入完整设备路径 ${LOCAL_DEVICE} 以确认: " confirm
    [ "$confirm" = "${LOCAL_DEVICE}" ] || err "用户取消烧录"

    # 需要 root 权限
    [ "$(id -u)" -eq 0 ] || err "dd 烧录需要 root 权限, 请使用 sudo"

    # 卸载已挂载的分区
    log "卸载 ${LOCAL_DEVICE} 上的分区"
    umount ${LOCAL_DEVICE}* 2>/dev/null || true

    # 烧录 idbloader
    log "dd 烧录 idbloader -> 偏移 ${OFFSET_IDBLOADER}"
    dd if="${IDBLOADER_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_IDBLOADER_DEC}" conv=notrunc

    # 烧录 u-boot
    log "dd 烧录 u-boot.itb -> 偏移 ${OFFSET_UBOOT}"
    dd if="${UBOOT_ITB}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_UBOOT_DEC}" conv=notrunc

    # 烧录 boot
    log "dd 烧录 boot.img -> 偏移 ${OFFSET_BOOT}"
    dd if="${BOOT_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_BOOT_DEC}" conv=notrunc

    # 烧录 rootfs
    log "dd 烧录 rootfs.img -> 偏移 ${OFFSET_ROOTFS}"
    dd if="${ROOTFS_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_ROOTFS_DEC}" conv=notrunc

    sync
    log "dd 烧录完成"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== eMMC 烧录开始 ==="
    check_inputs

    case "${FLASH_METHOD:-usb}" in
        usb) flash_via_upgrade_tool || err "USB 烧录工具不可用；不会自动回退到 dd" ;;
        dd)  flash_via_dd ;;
        *)   err "FLASH_METHOD 仅支持 usb 或 dd" ;;
    esac

    log "=== eMMC 烧录结束 ==="
    log "请重启开发板验证"
}

main "$@"
