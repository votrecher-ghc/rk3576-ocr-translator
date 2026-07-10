#!/bin/bash
# =====================================================================
# DMA-BUF 跟踪脚本
# 功能: 启用 ftrace 跟踪 DMA-BUF 分配/释放, 监控零拷贝链路
# 场景: 排查 V4L2 -> RGA -> DRM 零拷贝过程中的内存泄漏
# =====================================================================
set -e

# ---------------------- 路径变量 ----------------------
TRACEFS="${TRACEFS:-/sys/kernel/debug/tracing}"
TRACE_OUT="/tmp/dmabuf_trace.log"
# 跟踪持续时间 (秒)
DURATION="${DURATION:-10}"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[DMATrace $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[DMATrace ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 检查 tracefs ----------------------
check_tracefs() {
    [ -d "${TRACEFS}" ] || err "未找到 tracefs: ${TRACEFS}, 请先运行 enable_debugfs.sh"
    [ "$(id -u)" -eq 0 ] || err "需要 root 权限, 请使用 sudo"
}

# ---------------------- 步骤2: 配置跟踪事件 ----------------------
setup_trace() {
    log "配置 DMA-BUF 跟踪事件"

    # 清空之前的事件
    echo 0 > "${TRACEFS}/events/enable" 2>/dev/null || true
    echo > "${TRACEFS}/trace" 2>/dev/null || true

    # 启用 DMA-BUF 相关事件
    local events=(
        "dma_buf"
        "drm"
        "v4l2"
    )
    for evt in "${events[@]}"; do
        local evt_dir="${TRACEFS}/events/${evt}"
        if [ -d "$evt_dir" ]; then
            echo 1 > "$evt_dir/enable" 2>/dev/null || log "警告: 启用 ${evt} 事件失败"
            log "  已启用事件: ${evt}"
        else
            log "  [--] 事件不存在: ${evt}"
        fi
    done

    # 设置当前 tracer 为 function (可选: function_graph)
    echo "function" > "${TRACEFS}/current_tracer" 2>/dev/null \
        || log "警告: 设置 tracer 失败"

    # 设置过滤器 (聚焦 dma_buf 相关)
    echo '*dma_buf*' > "${TRACEFS}/set_ftrace_filter" 2>/dev/null || true

    log "跟踪事件配置完成"
}

# ---------------------- 步骤3: 开始跟踪 ----------------------
start_trace() {
    log "开始跟踪, 持续 ${DURATION} 秒..."
    echo 1 > "${TRACEFS}/tracing_on"

    # 等待指定时长
    sleep "${DURATION}"

    echo 0 > "${TRACEFS}/tracing_on"
    log "跟踪结束"
}

# ---------------------- 步骤4: 收集并分析结果 ----------------------
collect_results() {
    log "收集跟踪结果到: ${TRACE_OUT}"
    cat "${TRACEFS}/trace" > "${TRACE_OUT}"

    # 输出统计摘要
    log "=== DMA-BUF 跟踪摘要 ==="
    log "总事件数: $(grep -c '^' "${TRACE_OUT}" 2>/dev/null || echo 0)"

    # 统计 dma_buf 操作
    log "dma_buf 操作统计:"
    echo "  alloc: $(grep -c 'dma_buf_export\|dma_buf_dynamic_attach' "${TRACE_OUT}" 2>/dev/null || echo 0)"
    echo "  map:   $(grep -c 'dma_buf_map_attachment' "${TRACE_OUT}" 2>/dev/null || echo 0)"
    echo "  free:  $(grep -c 'dma_buf_release\|dma_buf_detach' "${TRACE_OUT}" 2>/dev/null || echo 0)"

    # 检查当前 dma_buf 缓冲区状态
    if [ -f "/sys/kernel/debug/dma_buf/bufinfo" ]; then
        log "当前 DMA-BUF 缓冲区状态:"
        cat /sys/kernel/debug/dma_buf/bufinfo | head -30
    fi

    log "完整跟踪日志: ${TRACE_OUT}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== DMA-BUF 跟踪开始 ==="
    check_tracefs
    setup_trace
    start_trace
    collect_results
    log "=== DMA-BUF 跟踪结束 ==="
}

main "$@"
