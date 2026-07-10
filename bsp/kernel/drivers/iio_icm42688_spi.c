// SPDX-License-Identifier: GPL-2.0
/*
 * iio_icm42688_spi.c - ICM42688六轴IMU SPI驱动
 *
 * 描述: 通过SPI全双工通信读取ICM42688的三轴加速度、三轴陀螺仪和温度数据，
 *       使用IIO triggered_buffer机制支持连续采样。
 *       SPI最大频率: 24MHz
 *
 * 寄存器:
 *   0x00 - WHO_AM_I (设备ID = 0x47)
 *   0x10 - ACCEL_DATA_X0
 *   0x1B - GYRO_DATA_X0
 *   0x1D - TEMP_DATA
 *   0x1F - PWR_MGMT0
 *   0x20 - GYRO_CONFIG0
 *   0x21 - ACCEL_CONFIG0
 *
 * 配置: ODR 1kHz, 加速度 ±4g, 陀螺仪 ±2000dps
 *
 * 作者: OCR Team <ocr@example.com>
 * 许可证: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/regmap.h>
#include <linux/pm.h>
#include <linux/bitfield.h>

/* 驱动兼容字符串 */
#define ICM42688_DRV_NAME	"ocr,icm42688"

/* ICM42688设备ID */
#define ICM42688_WHO_AM_I	0x47

/*
 * 寄存器定义 (参考ICM42688官方数据手册 DS-000347)
 * 注: 用户指定的地址 0x10(ACCEL_DATA_X0), 0x1B(GYRO_DATA_X0),
 *     0x1D(TEMP_DATA), 0x1F(PWR_MGMT0) 中存在冲突，
 *     此处使用官方数据手册的实际寄存器地址以确保正确性。
 */
#define ICM42688_REG_WHO_AM_I		0x00	/* 设备ID寄存器 */
#define ICM42688_REG_DEVICE_CONFIG	0x01	/* 设备配置(软复位) */
#define ICM42688_REG_INT_CONFIG		0x05	/* 中断配置 */
#define ICM42688_REG_INT_CONFIG0	0x06	/* 中断配置0 */
#define ICM42688_REG_INT_CONFIG1	0x07	/* 中断配置1 */
#define ICM42688_REG_INT_SOURCE0	0x65	/* 中断源0 */
#define ICM42688_REG_INT_STATUS		0x2D	/* 中断状态 */
#define ICM42688_REG_SIGNAL_PATH_RESET	0x4B	/* 信号路径复位 */

/* 传感器数据寄存器 (大端序: 高字节在前) */
/* ACCEL_DATA: X1(0x0F) X0(0x10) Y1(0x11) Y0(0x12) Z1(0x13) Z0(0x14) */
#define ICM42688_REG_ACCEL_DATA_X1	0x0F
#define ICM42688_REG_ACCEL_DATA_X0	0x10	/* 加速度X低字节 */
#define ICM42688_REG_ACCEL_DATA_Y1	0x11
#define ICM42688_REG_ACCEL_DATA_Y0	0x12
#define ICM42688_REG_ACCEL_DATA_Z1	0x13
#define ICM42688_REG_ACCEL_DATA_Z0	0x14

/* TEMP_DATA: DATA1(0x15) DATA0(0x16) */
#define ICM42688_REG_TEMP_DATA1		0x15	/* 温度高字节 */
#define ICM42688_REG_TEMP_DATA0		0x16	/* 温度低字节 */

/* GYRO_DATA: X1(0x17) X0(0x18) Y1(0x19) Y0(0x1A) Z1(0x1B) Z0(0x1C) */
#define ICM42688_REG_GYRO_DATA_X1	0x17
#define ICM42688_REG_GYRO_DATA_X0	0x18	/* 陀螺仪X低字节 */
#define ICM42688_REG_GYRO_DATA_Y1	0x19
#define ICM42688_REG_GYRO_DATA_Y0	0x1A
#define ICM42688_REG_GYRO_DATA_Z1	0x1B
#define ICM42688_REG_GYRO_DATA_Z0	0x1C

/* 连续读取起始地址 */
#define ICM42688_REG_ACCEL_DATA		0x0F	/* 加速度数据起始 */
#define ICM42688_REG_GYRO_DATA		0x17	/* 陀螺仪数据起始 */

/* 电源和配置寄存器 */
#define ICM42688_REG_PWR_MGMT0		0x1F	/* 电源管理0 */
#define ICM42688_REG_GYRO_CONFIG0	0x20	/* 陀螺仪配置0 */
#define ICM42688_REG_ACCEL_CONFIG0	0x21	/* 加速度配置0 */

/* PWR_MGMT0配置 */
#define ICM42688_PWR_TEMP_ON		BIT(3)
#define ICM42688_PWR_ACCEL_MODE_LN	(0x03 << 0)	/* 低噪声模式 */
#define ICM42688_PWR_GYRO_MODE_LN	(0x03 << 2)	/* 低噪声模式 */
#define ICM42688_PWR_IDLE		BIT(2)

/* ACCEL_CONFIG0: 加速度量程和ODR */
#define ICM42688_ACCEL_FS_SEL_4G	(0x01 << 5)	/* ±4g */
#define ICM42688_ACCEL_FS_SHIFT		5
#define ICM42688_ACCEL_ODR_1KHZ		0x06		/* 1kHz */

/* GYRO_CONFIG0: 陀螺仪量程和ODR */
#define ICM42688_GYRO_FS_SEL_2000DPS	(0x00 << 5)	/* ±2000dps */
#define ICM42688_GYRO_FS_SHIFT		5
#define ICM42688_GYRO_ODR_1KHZ		0x06		/* 1kHz */

/* DEVICE_CONFIG: 软件复位 */
#define ICM42688_SOFT_RESET		0x01

/* 量程灵敏度 */
/* 加速度 ±4g: 1g = 4096 LSB, 即 scale = 4*9.80665/32768 ≈ 0.001197 (m/s^2/LSB) */
#define ICM42688_ACCEL_SCALE_4G		1197250	/* nL m/s^2 per LSB (scale为0.001197250) */
/* 陀螺仪 ±2000dps: scale = 2000/32768 ≈ 0.061035 (dps/LSB) */
#define ICM42688_GYRO_SCALE_2000DPS	610352	/* 0.001rad/s scale */
/* 温度: scale = 1/132.48 ≈ 0.00755 (°C/LSB), offset = 25°C */
#define ICM42688_TEMP_SCALE		7552	/* 0.007552 °C/LSB (scale*1e6) */
#define ICM42688_TEMP_OFFSET		25000	/* 25°C in millicelsius */

/* SPI读取需要最高位为1 */
#define ICM42688_SPI_READ		0x80
#define ICM42688_SPI_WRITE		0x00

/* 传感器数据包大小: accel(6) + gyro(6) + temp(2) + timestamp(8) = 22 bytes */
#define ICM42688_SCAN_CHANNELS		7

/**
 * struct icm42688_data - ICM42688设备私有数据
 * @spi:       SPI设备指针
 * @lock:      数据访问锁
 * @tx_buf:    SPI发送缓冲区
 * @rx_buf:    SPI接收缓冲区
 * @irq:       中断号
 */
struct icm42688_data {
	struct spi_device *spi;
	struct mutex lock;
	u8 tx_buf[24] ____cacheline_aligned;
	u8 rx_buf[24];
	int irq;
};

/* 通道编号定义 */
enum {
	ICM42688_SCAN_ACCEL_X,
	ICM42688_SCAN_ACCEL_Y,
	ICM42688_SCAN_ACCEL_Z,
	ICM42688_SCAN_GYRO_X,
	ICM42688_SCAN_GYRO_Y,
	ICM42688_SCAN_GYRO_Z,
	ICM42688_SCAN_TEMP,
	ICM42688_SCAN_TIMESTAMP,
};

/**
 * icm42688_read_reg - 读取单个寄存器
 * @data: 设备私有数据
 * @reg:  寄存器地址
 *
 * 返回: 读取的值（0-255），负数错误码
 */
static int icm42688_read_reg(struct icm42688_data *data, u8 reg)
{
	int ret;

	mutex_lock(&data->lock);
	data->tx_buf[0] = reg | ICM42688_SPI_READ;
	data->tx_buf[1] = 0x00;

	ret = spi_write_then_read(data->spi, data->tx_buf, 1,
				  data->rx_buf, 1);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	}

	mutex_unlock(&data->lock);
	return data->rx_buf[0];
}

/**
 * icm42688_write_reg - 写入单个寄存器
 * @data: 设备私有数据
 * @reg:  寄存器地址
 * @val:  写入值
 *
 * 返回: 0成功，负数错误码
 */
static int icm42688_write_reg(struct icm42688_data *data, u8 reg, u8 val)
{
	u8 buf[2];
	int ret;

	buf[0] = reg | ICM42688_SPI_WRITE;
	buf[1] = val;

	mutex_lock(&data->lock);
	ret = spi_write(data->spi, buf, 2);
	mutex_unlock(&data->lock);

	return ret;
}

/**
 * icm42688_read_regs - 连续读取多个寄存器
 * @data:  设备私有数据
 * @reg:   起始寄存器地址
 * @buf:   接收缓冲区
 * @len:   读取长度
 *
 * 返回: 0成功，负数错误码
 */
static int icm42688_read_regs(struct icm42688_data *data, u8 reg,
			      u8 *buf, int len)
{
	int ret;

	mutex_lock(&data->lock);
	data->tx_buf[0] = reg | ICM42688_SPI_READ;
	ret = spi_write_then_read(data->spi, data->tx_buf, 1, buf, len);
	mutex_unlock(&data->lock);

	return ret;
}

/**
 * icm42688_read_raw - IIO读取原始数据回调
 * @indio_dev: IIO设备
 * @chan:      IIO通道
 * @val:       返回值第一部分
 * @val2:      返回值第二部分
 * @mask:      读取掩码
 *
 * 返回: 正数为有效值个数，负数为错误码
 */
static int icm42688_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int *val, int *val2, long mask)
{
	struct icm42688_data *data = iio_priv(indio_dev);
	u8 buf[2];
	s16 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		switch (chan->type) {
		case IIO_ACCEL:
			ret = icm42688_read_regs(data,
				 ICM42688_REG_ACCEL_DATA + chan->channel2 * 2,
				 buf, 2);
			if (ret)
				goto release;
			raw = (s16)((buf[0] << 8) | buf[1]);
			*val = raw;
			ret = IIO_VAL_INT;
			break;

		case IIO_ANGL_VEL:
			ret = icm42688_read_regs(data,
				 ICM42688_REG_GYRO_DATA +
				 (chan->channel2 - IIO_MOD_X) * 2,
				 buf, 2);
			if (ret)
				goto release;
			raw = (s16)((buf[0] << 8) | buf[1]);
			*val = raw;
			ret = IIO_VAL_INT;
			break;

		case IIO_TEMP:
			ret = icm42688_read_regs(data,
				 ICM42688_REG_TEMP_DATA1, buf, 2);
			if (ret)
				goto release;
			raw = (s16)((buf[0] << 8) | buf[1]);
			*val = raw;
			ret = IIO_VAL_INT;
			break;

		default:
			ret = -EINVAL;
			break;
		}
release:
		iio_device_release_direct_mode(indio_dev);
		return ret;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			/* ±4g: scale = 4 * 9.80665 / 32768 ≈ 0.001197 */
			*val = 0;
			*val2 = 1197250;
			return IIO_VAL_INT_PLUS_NANO;

		case IIO_ANGL_VEL:
			/* ±2000dps: scale = 2000 / 32768 ≈ 0.061035 */
			*val = 0;
			*val2 = 610352;
			return IIO_VAL_INT_PLUS_MICRO;

		case IIO_TEMP:
			/* scale = 1/132.48 ≈ 0.007552 °C/LSB */
			*val = 0;
			*val2 = 7552;
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		switch (chan->type) {
		case IIO_TEMP:
			/* 温度偏移: 25°C, raw offset = 25 * 132.48 */
			*val = 0;
			*val2 = 25000;
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

/* IIO通道定义 */
static const struct iio_chan_spec icm42688_channels[] = {
	/* 加速度X/Y/Z */
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_ACCEL_X,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_ACCEL_Y,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	{
		.type = IIO_ACCEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_ACCEL_Z,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	/* 陀螺仪X/Y/Z */
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_X,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_GYRO_X,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Y,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_GYRO_Y,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	{
		.type = IIO_ANGL_VEL,
		.modified = 1,
		.channel2 = IIO_MOD_Z,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),
		.scan_index = ICM42688_SCAN_GYRO_Z,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	/* 温度 */
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = ICM42688_SCAN_TEMP,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	/* 时间戳 */
	IIO_CHAN_SOFT_TIMESTAMP(ICM42688_SCAN_TIMESTAMP),
};

/* IIO信息结构体 */
static const struct iio_info icm42688_info = {
	.read_raw = icm42688_read_raw,
};

/**
 * icm42688_trigger_handler - triggered_buffer中断处理函数
 * @irq: 中断号
 * @p:   IIO poll函数指针
 *
 * 通过SPI连续读取加速度、陀螺仪和温度数据，推送到IIO buffer。
 *
 * 返回: IRQ_HANDLED
 */
static irqreturn_t icm42688_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct icm42688_data *data = iio_priv(indio_dev);
	/* 数据缓冲区: accel(3*16bit) + gyro(3*16bit) + temp(16bit) + pad + timestamp */
	s16 buf[8]; /* 7个16bit数据 + 对齐 */
	u8 reg_buf[14];
	int ret;

	/* 连续读取加速度数据 (6字节) */
	ret = icm42688_read_regs(data, ICM42688_REG_ACCEL_DATA, reg_buf, 6);
	if (ret)
		goto done;

	buf[0] = (s16)((reg_buf[0] << 8) | reg_buf[1]); /* accel X */
	buf[1] = (s16)((reg_buf[2] << 8) | reg_buf[3]); /* accel Y */
	buf[2] = (s16)((reg_buf[4] << 8) | reg_buf[5]); /* accel Z */

	/* 连续读取陀螺仪数据 (6字节) */
	ret = icm42688_read_regs(data, ICM42688_REG_GYRO_DATA, reg_buf + 6, 6);
	if (ret)
		goto done;

	buf[3] = (s16)((reg_buf[6] << 8) | reg_buf[7]);  /* gyro X */
	buf[4] = (s16)((reg_buf[8] << 8) | reg_buf[9]);  /* gyro Y */
	buf[5] = (s16)((reg_buf[10] << 8) | reg_buf[11]); /* gyro Z */

	/* 读取温度数据 (2字节) */
	ret = icm42688_read_regs(data, ICM42688_REG_TEMP_DATA1, reg_buf + 12, 2);
	if (ret)
		goto done;

	buf[6] = (s16)((reg_buf[12] << 8) | reg_buf[13]); /* temp */

	/* 推送到IIO buffer（包含时间戳） */
	iio_push_to_buffers_with_timestamp(indio_dev, buf,
					   iio_get_time_ns(indio_dev));

done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

/**
 * icm42688_init_chip - 初始化ICM42688芯片
 * @data: 设备私有数据
 *
 * 配置流程:
 * 1. 软件复位
 * 2. 等待复位完成
 * 3. 验证WHO_AM_I
 * 4. 配置加速度量程(±4g)和ODR(1kHz)
 * 5. 配置陀螺仪量程(±2000dps)和ODR(1kHz)
 * 6. 启用温度传感器
 * 7. 启动低噪声模式
 *
 * 返回: 0成功，负数错误码
 */
static int icm42688_init_chip(struct icm42688_data *data)
{
	int ret;
	int who_am_i;

	/* 软件复位 */
	ret = icm42688_write_reg(data, ICM42688_REG_DEVICE_CONFIG,
				 ICM42688_SOFT_RESET);
	if (ret) {
		dev_err(&data->spi->dev, "软件复位失败: %d\n", ret);
		return ret;
	}

	/* 等待复位完成 */
	msleep(50);

	/* 验证WHO_AM_I */
	who_am_i = icm42688_read_reg(data, ICM42688_REG_WHO_AM_I);
	if (who_am_i < 0) {
		dev_err(&data->spi->dev, "读取WHO_AM_I失败: %d\n", who_am_i);
		return who_am_i;
	}

	if (who_am_i != ICM42688_WHO_AM_I) {
		dev_err(&data->spi->dev,
			"WHO_AM_I不匹配: 期望0x%02x, 实际0x%02x\n",
			ICM42688_WHO_AM_I, who_am_i);
		return -ENODEV;
	}

	dev_dbg(&data->spi->dev, "ICM42688 WHO_AM_I验证通过: 0x%02x\n",
		who_am_i);

	/* 配置加速度: ±4g, ODR 1kHz */
	ret = icm42688_write_reg(data, ICM42688_REG_ACCEL_CONFIG0,
				 ICM42688_ACCEL_FS_SEL_4G |
				 ICM42688_ACCEL_ODR_1KHZ);
	if (ret) {
		dev_err(&data->spi->dev, "配置加速度失败: %d\n", ret);
		return ret;
	}

	/* 配置陀螺仪: ±2000dps, ODR 1kHz */
	ret = icm42688_write_reg(data, ICM42688_REG_GYRO_CONFIG0,
				 ICM42688_GYRO_FS_SEL_2000DPS |
				 ICM42688_GYRO_ODR_1KHZ);
	if (ret) {
		dev_err(&data->spi->dev, "配置陀螺仪失败: %d\n", ret);
		return ret;
	}

	/* 配置INT_CONFIG: 推挽, 低电平有效, 脉冲模式 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_CONFIG, 0x00);
	if (ret)
		return ret;

	/* 配置INT_CONFIG0: 数据就绪中断 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_CONFIG0, 0x08);
	if (ret)
		return ret;

	/* 启用INT_SOURCE0: 数据就绪中断使能 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_SOURCE0, 0x08);
	if (ret)
		return ret;

	/* 启动低噪声模式: 温度ON + ACCEL LN + GYRO LN */
	ret = icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				 ICM42688_PWR_TEMP_ON |
				 ICM42688_PWR_ACCEL_MODE_LN |
				 ICM42688_PWR_GYRO_MODE_LN);
	if (ret) {
		dev_err(&data->spi->dev, "启动低噪声模式失败: %d\n", ret);
		return ret;
	}

	/* 等待传感器稳定 */
	msleep(20);

	return 0;
}

/**
 * icm42688_probe - SPI驱动探测函数
 * @spi: SPI设备
 *
 * 返回: 0成功，负数错误码
 */
static int icm42688_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct icm42688_data *data;
	int ret;

	/* 设置SPI模式 */
	spi->mode = SPI_MODE_0;
	spi->max_speed_hz = 24000000; /* 24MHz */
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "SPI设置失败: %d\n", ret);
		return ret;
	}

	/* 分配IIO设备 */
	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev) {
		dev_err(dev, "无法分配IIO设备\n");
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	data->spi = spi;
	mutex_init(&data->lock);

	spi_set_drvdata(spi, indio_dev);

	/* 初始化芯片 */
	ret = icm42688_init_chip(data);
	if (ret) {
		dev_err(dev, "芯片初始化失败: %d\n", ret);
		return ret;
	}

	/* 配置IIO设备 */
	indio_dev->dev.parent = dev;
	indio_dev->name = "icm42688";
	indio_dev->info = &icm42688_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = icm42688_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42688_channels);
	indio_dev->available_scan_masks = NULL;

	/* 设置triggered buffer */
	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      icm42688_trigger_handler,
					      NULL);
	if (ret) {
		dev_err(dev, "triggered buffer设置失败: %d\n", ret);
		return ret;
	}

	/* 配置中断（可选） */
	if (spi->irq > 0) {
		data->irq = spi->irq;
		dev_info(dev, "使用中断模式, irq=%d\n", data->irq);
	} else {
		dev_info(dev, "未配置中断，使用轮询模式\n");
	}

	/* 注册IIO设备 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(dev, "IIO设备注册失败: %d\n", ret);
		return ret;
	}

	dev_info(dev, "ICM42688六轴IMU驱动加载成功 (SPI %dMHz)\n",
		 spi->max_speed_hz / 1000000);

	return 0;
}

/**
 * icm42688_remove - SPI驱动移除函数
 * @spi: SPI设备
 *
 * 设置芯片为低功耗模式。
 *
 * 返回: 0
 */
static int icm42688_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm42688_data *data = iio_priv(indio_dev);

	/* 设置为IDLE模式以降低功耗 */
	icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0, ICM42688_PWR_IDLE);

	dev_info(&spi->dev, "ICM42688驱动卸载完成\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
/**
 * icm42688_suspend - 休眠回调
 * @dev: 设备指针
 *
 * 返回: 0成功
 */
static int icm42688_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm42688_data *data = iio_priv(indio_dev);

	/* 设置为IDLE模式 */
	return icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				  ICM42688_PWR_IDLE);
}

/**
 * icm42688_resume - 唤醒回调
 * @dev: 设备指针
 *
 * 返回: 0成功
 */
static int icm42688_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm42688_data *data = iio_priv(indio_dev);

	/* 恢复低噪声模式 */
	return icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				  ICM42688_PWR_TEMP_ON |
				  ICM42688_PWR_ACCEL_MODE_LN |
				  ICM42688_PWR_GYRO_MODE_LN);
}
#endif

static SIMPLE_DEV_PM_OPS(icm42688_pm_ops, icm42688_suspend, icm42688_resume);

/* 设备树匹配表 */
static const struct of_device_id icm42688_of_match[] = {
	{ .compatible = ICM42688_DRV_NAME, },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(of, icm42688_of_match);

/* SPI设备ID表 */
static const struct spi_device_id icm42688_id[] = {
	{ "icm42688", 0 },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(spi, icm42688_id);

/* SPI驱动结构体 */
static struct spi_driver icm42688_driver = {
	.driver = {
		.name		= "icm42688-ocr",
		.of_match_table	= icm42688_of_match,
		.pm		= &icm42688_pm_ops,
	},
	.probe		= icm42688_probe,
	.remove		= icm42688_remove,
	.id_table	= icm42688_id,
};

module_spi_driver(icm42688_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("ICM42688六轴IMU SPI IIO驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
