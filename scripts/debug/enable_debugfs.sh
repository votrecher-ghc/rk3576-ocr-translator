#!/bin/bash
# =====================================================================
# 挂载 debugfs 调试文件系统脚本
# 功能: 挂载 debugfs 并常用调试节点路径, 便于排查 DRM / V4L2 / DMA-BUF
# =====================================================================
set -e

# ---------------------- 路径变量 ----------------------
DEBUGFS_MNT="${DEBUGFS_MNT:-/sys/kernel/debug}"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[Debugfs $(date '+%H:%M:%S')] $*"
}

# ---------------------- 步骤1: 挂载 debugfs ----------------------
mount_debugfs() {
    if mountpoint -q "${DEBUGFS_MNT}" 2>/dev/null; then
        log "debugfs 已挂载: ${DEBUGFS_MNT}"
    else
        log "挂载 debugfs 到 ${DEBUGFS_MNT}"
        mount -t debugfs none "${DEBUGFS_MNT}" \
            || { echo "请使用 root 权限运行"; exit 1; }
        log "debugfs 挂载成功"
    fi
}

# ---------------------- 步骤2: 检查常用调试节点 ----------------------
check_debug_nodes() {
    log "检查常用调试节点:"
    local nodes=(
        "drm"              # DRM 子系统
        "dma_buf"          # DMA-BUF 跟踪
        "v4l2"             # V4L2 视频
        "rockchip/rga"     # RGA 2D 加速
        "rknn"             # RKNN NPU (如驱动暴露)
        "regulator"        # 电源调节器
        "clk"              # 时钟树
        "pinctrl"          # 引脚控制
    )
    for node in "${nodes[@]}"; do
        if [ -e "${DEBUGFS_MNT}/${node}" ]; then
            log "  [OK] ${node}"
        else
            log "  [--] ${node} (不存在)"
        fi
    done
}

# ---------------------- 步骤3: 输出常用查询命令 ----------------------
print_help() {
    log "=== 常用调试查询命令 ==="
    echo "# 查看 DRM 帧缓冲状态:"
    echo "  cat ${DEBUGFS_MNT}/drm/0/framebuffer"
    echo ""
    echo "# 查看 DMA-BUF 统计:"
    echo "  cat ${DEBUGFS_MNT}/dma_buf/bufinfo"
    echo ""
    echo "# 启用 V4L2 跟踪事件:"
    echo "  echo 1 > ${DEBUGFS_MNT}/tracing/events/v4l2/enable"
    echo ""
    echo "# 查看当前跟踪日志:"
    echo "  cat ${DEBUGFS_MNT}/tracing/trace"
    echo ""
    echo "# 查看 RGA 状态:"
    echo "  cat ${DEBUGFS_MNT}/rockchip/rga/load 2>/dev/null"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== 挂载 debugfs 开始 ==="
    mount_debugfs
    check_debug_nodes
    print_help
    log "=== 完成 ==="
}

main "$@"
