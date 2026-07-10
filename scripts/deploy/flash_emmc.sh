#!/bin/bash
# =====================================================================
# eMMC 烧录脚本 (野火 LubanCat3 / RK3576)
# 功能: 将编译好的固件烧录到开发板 eMMC
# 方式: 优先使用 Rockchip upgrade_tool (USB 烧录), 备选 dd 本地烧录
# =====================================================================
set -e

# ---------------------- 路径与设备变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
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
LOCAL_DEVICE="${LOCAL_DEVICE:-/dev/mmcblk0}"

# 分区偏移 (单位: 扇区, 512B)
OFFSET_IDBLOADER=64
OFFSET_UBOOT=58000
OFFSET_BOOT=16384
OFFSET_ROOTFS=262144

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Flash $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[Flash ERROR] $*" >&2
    exit 1
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
        # 分区烧录
        log "烧录 idbloader (偏移: ${OFFSET_IDBLOADER})"
        "${UPGRADE_TOOL}" wl "${OFFSET_IDBLOADER}" "${IDBLOADER_IMG}" \
            || err "idbloader 烧录失败"

        log "烧录 u-boot.itb (偏移: ${OFFSET_UBOOT})"
        "${UPGRADE_TOOL}" wl "${OFFSET_UBOOT}" "${UBOOT_ITB}" \
            || err "u-boot 烧录失败"

        log "烧录 boot.img (偏移: ${OFFSET_BOOT})"
        "${UPGRADE_TOOL}" wl "${OFFSET_BOOT}" "${BOOT_IMG}" \
            || err "boot.img 烧录失败"

        log "烧录 rootfs.img (偏移: ${OFFSET_ROOTFS})"
        "${UPGRADE_TOOL}" wl "${OFFSET_ROOTFS}" "${ROOTFS_IMG}" \
            || err "rootfs 烧录失败"
    fi

    log "USB 烧录完成"
    return 0
}

# ---------------------- 步骤3: 本地 dd 烧录 (备选) ----------------------
flash_via_dd() {
    log "警告: 使用 dd 本地烧录方式"
    log "目标设备: ${LOCAL_DEVICE}"
    log "请确认设备路径正确, 误烧将导致数据丢失!"
    read -p "确认烧录到 ${LOCAL_DEVICE}? (输入 yes 继续): " confirm
    [ "$confirm" = "yes" ] || err "用户取消烧录"

    # 需要 root 权限
    [ "$(id -u)" -eq 0 ] || err "dd 烧录需要 root 权限, 请使用 sudo"

    # 卸载已挂载的分区
    log "卸载 ${LOCAL_DEVICE} 上的分区"
    umount ${LOCAL_DEVICE}* 2>/dev/null || true

    # 烧录 idbloader
    log "dd 烧录 idbloader -> 偏移 ${OFFSET_IDBLOADER}"
    dd if="${IDBLOADER_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_IDBLOADER}" conv=notrunc

    # 烧录 u-boot
    log "dd 烧录 u-boot.itb -> 偏移 ${OFFSET_UBOOT}"
    dd if="${UBOOT_ITB}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_UBOOT}" conv=notrunc

    # 烧录 boot
    log "dd 烧录 boot.img -> 偏移 ${OFFSET_BOOT}"
    dd if="${BOOT_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_BOOT}" conv=notrunc

    # 烧录 rootfs
    log "dd 烧录 rootfs.img -> 偏移 ${OFFSET_ROOTFS}"
    dd if="${ROOTFS_IMG}" of="${LOCAL_DEVICE}" \
        bs=512 seek="${OFFSET_ROOTFS}" conv=notrunc

    sync
    log "dd 烧录完成"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== eMMC 烧录开始 ==="
    check_inputs

    # 优先 USB 烧录, 失败则回退 dd 方式
    if ! flash_via_upgrade_tool; then
        flash_via_dd
    fi

    log "=== eMMC 烧录结束 ==="
    log "请重启开发板验证"
}

main "$@"
