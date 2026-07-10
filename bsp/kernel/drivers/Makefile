# SPDX-License-Identifier: GPL-2.0
#
# OCR系统自研驱动 Makefile
#
# 描述: 编译RK3576鲁班猫3 OCR系统的5个自研内核驱动为内核模块
#
# 作者: OCR Team <ocr@example.com>
# 许可证: GPL-2.0
#

# OCR系统自研驱动配置选项
# CONFIG_OCR_GPIO_KEYS   - GPIO按键驱动
# CONFIG_IIO_AP3216C     - AP3216C环境光/接近传感器
# CONFIG_IIO_ICM42688_SPI - ICM42688六轴IMU
# CONFIG_IIO_ADT7410     - ADT7410温度传感器
# CONFIG_PWM_FAN_OCR     - PWM风扇温控

# GPIO按键驱动 (GPIO + Input子系统)
obj-$(CONFIG_OCR_GPIO_KEYS)	+= gpio_keys_ocr.o

# AP3216C环境光/接近传感器 (I2C + IIO)
obj-$(CONFIG_IIO_AP3216C)	+= iio_ap3216c.o

# ICM42688六轴IMU (SPI + IIO)
obj-$(CONFIG_IIO_ICM42688_SPI)	+= iio_icm42688_spi.o

# ADT7410温度传感器 (I2C + IIO)
obj-$(CONFIG_IIO_ADT7410)	+= iio_adt7410.o

# PWM风扇温控驱动 (PWM + hwmon)
obj-$(CONFIG_PWM_FAN_OCR)	+= pwm_fan_ocr.o
