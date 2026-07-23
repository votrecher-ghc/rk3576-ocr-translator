# SPDX-License-Identifier: GPL-2.0

obj-m += pwm_fan_ocr.o

KDIR ?= $(CURDIR)/../../kernel/src
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

.PHONY: all modules clean

all: modules

modules:
	$(MAKE) -C $(KDIR) \
		M=$(CURDIR) \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE) \
		modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
