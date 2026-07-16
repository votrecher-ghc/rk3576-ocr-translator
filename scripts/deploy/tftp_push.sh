#!/bin/bash
# =====================================================================
# TFTP 推送脚本 (内核和 DTB 推送到开发板)
# 功能: 通过 TFTP 将编译好的内核 Image 和 DTB 推送到目标板
# 使用场景: 开发阶段快速更新内核, 无需重新烧录
# =====================================================================
set -e

# ---------------------- 路径与网络变量 ----------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
KERNEL_OUT="${PROJECT_ROOT}/output/kernel"

# TFTP 服务器配置 (可在环境变量覆盖)
TFTP_SERVER="${TFTP_SERVER:-192.168.1.100}"
TFTP_PORT="${TFTP_PORT:-69}"

# 目标板 IP 地址
BOARD_IP="${BOARD_IP:-192.168.1.200}"

# 推送的文件
KERNEL_IMAGE="${KERNEL_OUT}/Image"
KERNEL_DTB="${KERNEL_OUT}/rk3576-lubancat3.dtb"
# 默认推送未经 overlay 修改的 vendor base DTB。只有完成原理图核对并由
# make_image.sh 生成合并 DTB 后，才应显式设置 TFTP_DTB 指向该文件。
TFTP_DTB="${TFTP_DTB:-${KERNEL_DTB}}"
DTBO_DIR="${KERNEL_OUT}/dtbo"
# Optional space-separated DTBO basenames to upload as standalone artifacts.
# Uploading does not apply them; U-Boot/vendor resource configuration must do so.
TFTP_DTBO_LIST="${TFTP_DTBO_LIST:-}"

# TFTP 远程目录
REMOTE_KERNEL_DIR="${REMOTE_KERNEL_DIR:-kernel}"
REMOTE_DTB_DIR="${REMOTE_DTB_DIR:-dtb}"

# ---------------------- 辅助函数 ----------------------
log() {
    echo -e "[TFTP $(date '+%H:%M:%S')] $*"
}

err() {
    echo -e "[TFTP ERROR] $*" >&2
    exit 1
}

# ---------------------- 步骤1: 检查文件与工具 ----------------------
check_prerequisites() {
    command -v tftp >/dev/null 2>&1 || command -v atftp >/dev/null 2>&1 \
        || err "未找到 tftp / atftp 客户端, 请安装"
    [ -f "${KERNEL_IMAGE}" ] || err "未找到内核 Image: ${KERNEL_IMAGE}"
    [ -f "${TFTP_DTB}" ] || err "未找到 DTB: ${TFTP_DTB}"
    log "输入文件检查通过"
}

# ---------------------- 步骤2: 推送内核 Image ----------------------
push_kernel() {
    local remote="${REMOTE_KERNEL_DIR#/}/Image"
    log "推送内核 Image -> ${TFTP_SERVER}:${remote}"
    # 使用 atftp (支持 PUT) 或 tftp
    if command -v atftp >/dev/null 2>&1; then
        atftp -p -l "${KERNEL_IMAGE}" -r "${remote}" "${TFTP_SERVER}" "${TFTP_PORT}"
    else
        # 部分系统需用 tftp-hpa
        tftp "${TFTP_SERVER}" "${TFTP_PORT}" -c put "${KERNEL_IMAGE}" "${remote}" \
            || err "推送 Image 失败, 请检查 TFTP 服务器配置"
    fi
    log "内核 Image 推送完成"
}

# ---------------------- 步骤3: 推送 DTB ----------------------
push_dtb() {
    local remote="${REMOTE_DTB_DIR#/}/rk3576-lubancat3.dtb"
    log "推送 DTB -> ${TFTP_SERVER}:${remote}"
    if command -v atftp >/dev/null 2>&1; then
        atftp -p -l "${TFTP_DTB}" -r "${remote}" "${TFTP_SERVER}" "${TFTP_PORT}"
    else
        tftp "${TFTP_SERVER}" "${TFTP_PORT}" -c put "${TFTP_DTB}" "${remote}" \
            || err "推送 DTB 失败"
    fi
    log "DTB 推送完成"
}

# ---------------------- 步骤4: 推送 dtbo overlay (如存在) ----------------------
push_dtbo() {
    if [ -z "${TFTP_DTBO_LIST}" ]; then
        log "未设置 TFTP_DTBO_LIST，默认不推送任何独立 dtbo"
        return
    fi

    local names=()
    local name dtbo remote
    read -r -a names <<< "${TFTP_DTBO_LIST}"
    for name in "${names[@]}"; do
        case "${name}" in
            *.dtbo) ;;
            *) err "TFTP_DTBO_LIST 只接受 .dtbo 文件名: ${name}" ;;
        esac
        case "${name}" in
            */*|*\\*) err "TFTP_DTBO_LIST 只接受文件名，不能包含路径: ${name}" ;;
        esac
        dtbo="${DTBO_DIR}/${name}"
        [ -f "${dtbo}" ] || err "未找到指定 dtbo: ${dtbo}"
        remote="${REMOTE_DTB_DIR#/}/${name}"
        log "  推送: ${name}"
        if command -v atftp >/dev/null 2>&1; then
            atftp -p -l "${dtbo}" -r "${remote}" "${TFTP_SERVER}" "${TFTP_PORT}"
        else
            tftp "${TFTP_SERVER}" "${TFTP_PORT}" -c put "${dtbo}" "${remote}" \
                || err "推送 ${name} 失败"
        fi
    done
    log "dtbo 推送完成（上传不等于启用）"
}

# ---------------------- 步骤5: 提示板端加载命令 ----------------------
print_board_cmd() {
    log "=== 推送完成 ==="
    echo "请在 U-Boot 命令行执行以下命令加载新内核:"
    echo "  => setenv serverip ${TFTP_SERVER}"
    echo "  => setenv ipaddr ${BOARD_IP}"
    echo "  => tftpboot \${kernel_addr_r} ${REMOTE_KERNEL_DIR}/Image"
    echo "  => tftpboot \${fdt_addr_r} ${REMOTE_DTB_DIR}/rk3576-lubancat3.dtb"
    echo "  => booti \${kernel_addr_r} - \${fdt_addr_r}"
}

# ---------------------- 主流程 ----------------------
main() {
    log "=== TFTP 推送开始 ==="
    check_prerequisites
    push_kernel
    push_dtb
    push_dtbo
    print_board_cmd
    log "=== TFTP 推送结束 ==="
}

main "$@"
