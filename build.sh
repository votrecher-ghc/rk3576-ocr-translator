#!/bin/bash
# 按目标调用各阶段脚本；all 顺序为 U-Boot、内核、应用、模型、rootfs 和固件打包。
# 这里是调度入口，各阶段的输入路径、依赖检查和产物收集由被调用脚本负责。
# ============================================================
# RK3576 OCR 翻译系统 - 顶层构建入口
# 用法: ./build.sh [target]
#   target: all | uboot | kernel | app | models | rootfs | image | clean
# ============================================================

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
TARGET="${1:-all}"

build_kernel() {
    echo ">>> Building Linux Kernel..."
    bash "${PROJECT_ROOT}/scripts/build/build_kernel.sh"
}

build_uboot() {
    echo ">>> Building U-Boot..."
    bash "${PROJECT_ROOT}/scripts/build/build_uboot.sh"
}

build_app() {
    echo ">>> Building Application..."
    bash "${PROJECT_ROOT}/scripts/build/build_app.sh"
}

build_models() {
    echo ">>> Converting RKNN Models..."
    bash "${PROJECT_ROOT}/scripts/build/build_models.sh"
}

build_rootfs() {
    echo ">>> Building Buildroot RootFS..."
    bash "${PROJECT_ROOT}/scripts/build/build_rootfs.sh"
}

make_image() {
    echo ">>> Packing firmware image..."
    bash "${PROJECT_ROOT}/scripts/build/make_image.sh"
}

clean_all() {
    echo ">>> Cleaning..."
    rm -rf "${PROJECT_ROOT}/app/build"
    rm -rf "${PROJECT_ROOT}/output"
    echo "Done."
}

case "${TARGET}" in
    all)
        build_uboot
        build_kernel
        build_app
        build_models
        build_rootfs
        make_image
        ;;
    uboot)   build_uboot ;;
    kernel)  build_kernel ;;
    app)     build_app ;;
    models)  build_models ;;
    rootfs)  build_rootfs ;;
    image)   make_image ;;
    clean)   clean_all ;;
    *)
        echo "Usage: $0 {all|uboot|kernel|app|models|rootfs|image|clean}"
        exit 1
        ;;
esac

echo ">>> Build complete: ${TARGET}"
