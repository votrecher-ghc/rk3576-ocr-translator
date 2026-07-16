# ============================================================
# RK3576 OCR 翻译系统 - 顶层 Makefile
# ============================================================

PROJECT_ROOT := $(shell pwd)
BUILD_DIR    := $(PROJECT_ROOT)/build
OUT_DIR      := $(PROJECT_ROOT)/output

# Cross compile settings
CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH          := arm64

# Tool paths
KERNEL_SRC  ?= $(PROJECT_ROOT)/bsp/kernel/src
UBOOT_SRC   ?= $(PROJECT_ROOT)/bsp/u-boot/src
BUILDROOT_SRC ?= $(PROJECT_ROOT)/bsp/buildroot/src
APP_DIR     := $(PROJECT_ROOT)/app

export CROSS_COMPILE ARCH KERNEL_SRC UBOOT_SRC BUILDROOT_SRC

.PHONY: all uboot kernel app models rootfs image clean help

all: uboot kernel app models rootfs image

help:
	@echo "Available targets:"
	@echo "  all      - Build everything"
	@echo "  uboot    - Build U-Boot"
	@echo "  kernel   - Build Linux kernel + DTB + modules"
	@echo "  app      - Build application (CMake)"
	@echo "  models   - Convert RKNN models"
	@echo "  rootfs   - Build Buildroot rootfs"
	@echo "  image    - Pack firmware image"
	@echo "  clean    - Clean build artifacts"

uboot:
	@bash scripts/build/build_uboot.sh

kernel:
	@bash scripts/build/build_kernel.sh

app:
	@bash scripts/build/build_app.sh

models:
	@bash scripts/build/build_models.sh

rootfs:
	@bash scripts/build/build_rootfs.sh

image:
	@bash scripts/build/make_image.sh

clean:
	@rm -rf $(BUILD_DIR) $(OUT_DIR) $(APP_DIR)/build
	@echo "Clean done."
