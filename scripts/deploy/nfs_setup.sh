#!/bin/bash
# =====================================================================
# NFS 根文件系统挂载配置脚本
# 功能: 配置主机端 NFS 服务, 供开发板通过 NFS 挂载根文件系统
# 使用场景: 开发阶段快速迭代 rootfs, 无需反复烧录 eMMC
# =====================================================================
set -e

# ---------------------- 路径与网络变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# NFS 根目录 (挂载源)
NFS_ROOT="${NFS_ROOT:-${PROJECT_ROOT}/output/rootfs/nfsroot}"
# 允许访问的网段
NFS_NETWORK="${NFS_NETWORK:-192.168.1.0/24}"
# NFS 导出配置文件
NFS_EXPORTS="/etc/exports"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[NFS $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[NFS ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 检查 NFS 服务 ----------------------
check_nfs_server() {
    if ! dpkg -l nfs-kernel-server >/dev/null 2>&1 && \
       ! rpm -q nfs-utils >/dev/null 2>&1; then
        log "警告: 未检测到 NFS 服务端包"
        log "请在 Ubuntu/Debian 安装: sudo apt install nfs-kernel-server"
        log "请在 CentOS/RHEL 安装: sudo yum install nfs-utils"
    fi
    log "NFS 服务检查完成"
}

# ---------------------- 步骤2: 准备 NFS 根目录 ----------------------
prepare_root() {
    mkdir -p "${NFS_ROOT}"
    log "NFS 根目录: ${NFS_ROOT}"

    # 若目录为空, 尝试解压 rootfs
    local ROOTFS_IMG="${PROJECT_ROOT}/output/rootfs/rootfs.ext4"
    if [ -z "$(ls -A "${NFS_ROOT}" 2>/dev/null)" ] && [ -f "${ROOTFS_IMG}" ]; then
        log "NFS 根目录为空, 尝试从 rootfs.ext4 提取"
        log "提示: 需要 root 权限挂载 ext4 镜像"
        local MNT_TMP="/tmp/ocr_rootfs_mnt"
        cleanup_rootfs_mount() {
            if mountpoint -q "${MNT_TMP}" 2>/dev/null; then
                sudo umount "${MNT_TMP}" || true
            fi
            rmdir "${MNT_TMP}" 2>/dev/null || true
        }
        trap cleanup_rootfs_mount EXIT INT TERM
        mkdir -p "${MNT_TMP}"
        sudo mount -o loop "${ROOTFS_IMG}" "${MNT_TMP}"
        sudo cp -a "${MNT_TMP}/." "${NFS_ROOT}/"
        sudo umount "${MNT_TMP}"
        rmdir "${MNT_TMP}"
        trap - EXIT INT TERM
        log "rootfs 已提取到 NFS 根目录"
    fi
}

# ---------------------- 步骤3: 配置 NFS 导出 ----------------------
configure_export() {
    local EXPORT_LINE="${NFS_ROOT} ${NFS_NETWORK}(rw,sync,no_subtree_check,no_root_squash)"

    log "配置 NFS 导出: ${EXPORT_LINE}"

    # 检查是否已存在导出项
    if grep -q "^${NFS_ROOT} " "${NFS_EXPORTS}" 2>/dev/null; then
        log "导出项已存在, 更新中..."
        sudo sed -i "\#^${NFS_ROOT} #c\\${EXPORT_LINE}" "${NFS_EXPORTS}"
    else
        echo "${EXPORT_LINE}" | sudo tee -a "${NFS_EXPORTS}" >/dev/null
    fi

    # 刷新导出
    sudo exportfs -ra
    log "NFS 导出配置完成"
}

# ---------------------- 步骤4: 重启 NFS 服务 ----------------------
restart_service() {
    log "重启 NFS 服务"
    if systemctl is-active nfs-kernel-server >/dev/null 2>&1; then
        sudo systemctl restart nfs-kernel-server
    elif systemctl is-active nfs-server >/dev/null 2>&1; then
        sudo systemctl restart nfs-server
    else
        sudo systemctl restart nfs-kernel-server 2>/dev/null \
            || sudo systemctl restart nfs-server 2>/dev/null \
            || err "NFS 服务重启失败, 请检查服务状态"
    fi
    log "NFS 服务已启动"
}

# ---------------------- 步骤5: 输出板端挂载命令 ----------------------
print_board_cmd() {
    local NFS_SERVER_IP
    NFS_SERVER_IP=$(hostname -I | awk '{print $1}')
    log "=== NFS 配置完成 ==="
    echo "开发板 U-Boot 启动参数 (NFS root):"
    echo "  => setenv serverip ${NFS_SERVER_IP}"
    echo "  => setenv ipaddr <开发板IP>"
    echo "  => setenv rootpath ${NFS_ROOT}"
    echo "  => setenv nfsroot ${NFS_SERVER_IP}:${NFS_ROOT}"
    echo "  => setenv bootargs root=/dev/nfs nfsroot=${NFS_SERVER_IP}:${NFS_ROOT} \\
         ip=<开发板IP>:${NFS_SERVER_IP}:<网关>:<掩码>::eth0 rw rootwait"
    echo "  => tftpboot \${kernel_addr_r} kernel/Image"
    echo "  => tftpboot \${fdt_addr_r} dtb/rk3576-lubancat3.dtb"
    echo "  => booti \${kernel_addr_r} - \${fdt_addr_r}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== NFS 配置开始 ==="
    check_nfs_server
    prepare_root
    configure_export
    restart_service
    print_board_cmd
    log "=== NFS 配置结束 ==="
}

main "$@"
