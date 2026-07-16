// SPDX-License-Identifier: GPL-2.0
/*
 * iio_icm42688_spi.c - ICM42688六轴IMU SPI驱动
 *
 * 描述: 通过SPI全双工通信读取ICM42688的三轴加速度、三轴陀螺仪和温度数据，
 *       使用IIO triggered_buffer机制支持连续采样。
 *       SPI最大频率: 24MHz
 *
 * 关键寄存器（TDK DS-000347）:
 *   0x75 - WHO_AM_I (设备ID = 0x47)
 *   0x1D - TEMP_DATA1；0x1F - ACCEL_DATA_X1；0x25 - GYRO_DATA_X1
 *   0x4E - PWR_MGMT0；0x4F - GYRO_CONFIG0；0x50 - ACCEL_CONFIG0
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
#include <linux/iio/trigger.h>
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
#include <linux/string.h>

/* 驱动兼容字符串 */
#define ICM42688_DRV_NAME	"ocr,icm42688"

/* ICM42688设备ID */
#define ICM42688_WHO_AM_I	0x47

/*
 * 寄存器定义 (参考ICM42688官方数据手册 DS-000347)
 * 所有地址均使用 TDK DS-000347 user bank 0 定义。
 */
#define ICM42688_REG_WHO_AM_I		0x75	/* 设备ID寄存器 */
#define ICM42688_REG_DEVICE_CONFIG	0x11	/* 设备配置(软复位) */
#define ICM42688_REG_INT_CONFIG		0x14	/* 中断配置 */
#define ICM42688_REG_INT_CONFIG0	0x63	/* 中断配置0 */
#define ICM42688_REG_INT_CONFIG1	0x64	/* 中断配置1 */
#define ICM42688_REG_INT_SOURCE0	0x65	/* 中断源0 */
#define ICM42688_REG_INT_STATUS		0x2D	/* 中断状态 */
#define ICM42688_REG_SIGNAL_PATH_RESET	0x4B	/* 信号路径复位 */

/* 传感器数据寄存器 (大端序: 高字节在前) */
/* ACCEL_DATA: X1(0x1F)..Z0(0x24) */
#define ICM42688_REG_ACCEL_DATA_X1	0x1F
#define ICM42688_REG_ACCEL_DATA_X0	0x20	/* 加速度X低字节 */
#define ICM42688_REG_ACCEL_DATA_Y1	0x21
#define ICM42688_REG_ACCEL_DATA_Y0	0x22
#define ICM42688_REG_ACCEL_DATA_Z1	0x23
#define ICM42688_REG_ACCEL_DATA_Z0	0x24

/* TEMP_DATA: DATA1(0x1D) DATA0(0x1E) */
#define ICM42688_REG_TEMP_DATA1		0x1D	/* 温度高字节 */
#define ICM42688_REG_TEMP_DATA0		0x1E	/* 温度低字节 */

/* GYRO_DATA: X1(0x25)..Z0(0x2A) */
#define ICM42688_REG_GYRO_DATA_X1	0x25
#define ICM42688_REG_GYRO_DATA_X0	0x26	/* 陀螺仪X低字节 */
#define ICM42688_REG_GYRO_DATA_Y1	0x27
#define ICM42688_REG_GYRO_DATA_Y0	0x28
#define ICM42688_REG_GYRO_DATA_Z1	0x29
#define ICM42688_REG_GYRO_DATA_Z0	0x2A

/* 连续读取起始地址 */
#define ICM42688_REG_ACCEL_DATA		0x1F	/* 加速度数据起始 */
#define ICM42688_REG_GYRO_DATA		0x25	/* 陀螺仪数据起始 */

/* 电源和配置寄存器 */
#define ICM42688_REG_PWR_MGMT0		0x4E	/* 电源管理0 */
#define ICM42688_REG_GYRO_CONFIG0	0x4F	/* 陀螺仪配置0 */
#define ICM42688_REG_ACCEL_CONFIG0	0x50	/* 加速度配置0 */

/* PWR_MGMT0配置 */
#define ICM42688_PWR_TEMP_DIS		BIT(5)		/* 关闭温度传感器 */
#define ICM42688_PWR_ACCEL_MODE_LN	(0x03 << 0)	/* 低噪声模式 */
#define ICM42688_PWR_GYRO_MODE_LN	(0x03 << 2)	/* 低噪声模式 */

/* ACCEL_CONFIG0: 加速度量程和ODR */
#define ICM42688_ACCEL_FS_SEL_4G	(0x02 << 5)	/* ±4g */
#define ICM42688_ACCEL_FS_SHIFT		5
#define ICM42688_ACCEL_ODR_1KHZ		0x06		/* 1kHz */

/* GYRO_CONFIG0: 陀螺仪量程和ODR */
#define ICM42688_GYRO_FS_SEL_2000DPS	(0x00 << 5)	/* ±2000dps */
#define ICM42688_GYRO_FS_SHIFT		5
#define ICM42688_GYRO_ODR_1KHZ		0x06		/* 1kHz */

/* DEVICE_CONFIG: 软件复位 */
#define ICM42688_SOFT_RESET		0x01

/* 量程灵敏度 */
/* 加速度 ±4g: 1g = 8192 LSB, 即 scale = 4*9.80665/32768 ≈ 0.001197101 m/s²/LSB */
#define ICM42688_ACCEL_SCALE_4G		1197101	/* nm/s² per LSB */
/* 陀螺仪 ±2000dps: 约 0.001065264 rad/s/LSB。 */
#define ICM42688_GYRO_SCALE_2000DPS	1065264	/* nrad/s per LSB */
/* IIO_TEMP uses millidegrees Celsius: (raw + 3312) * 7.548309. */
#define ICM42688_TEMP_SCALE_MICRO	7548309
#define ICM42688_TEMP_OFFSET_RAW	3312

/* SPI读取需要最高位为1 */
#define ICM42688_SPI_READ		0x80
#define ICM42688_SPI_WRITE		0x00

/* 7 个 16-bit 通道 + 2-byte pad + 64-bit timestamp = 24 bytes。 */
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
	struct iio_trigger *trig;
};

struct icm42688_scan {
	__be16 channels[ICM42688_SCAN_CHANNELS];
	__be16 pad;
	s64 timestamp;
} __aligned(8);

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

static int icm42688_update_bits(struct icm42688_data *data, u8 reg,
				u8 mask, u8 value)
{
	int current = icm42688_read_reg(data, reg);

	if (current < 0)
		return current;
	current = (current & ~mask) | (value & mask);
	return icm42688_write_reg(data, reg, (u8)current);
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
			*val2 = ICM42688_ACCEL_SCALE_4G;
			return IIO_VAL_INT_PLUS_NANO;

		case IIO_ANGL_VEL:
			/* IIO angular velocity ABI is rad/s, not degrees/s. */
			*val = 0;
			*val2 = ICM42688_GYRO_SCALE_2000DPS;
			return IIO_VAL_INT_PLUS_NANO;

		case IIO_TEMP:
			/* IIO temperature scale is expressed in millidegrees. */
			*val = ICM42688_TEMP_SCALE_MICRO / 1000000;
			*val2 = ICM42688_TEMP_SCALE_MICRO % 1000000;
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		switch (chan->type) {
		case IIO_TEMP:
			/* (raw + offset) * scale = millidegrees Celsius. */
			*val = ICM42688_TEMP_OFFSET_RAW;
			return IIO_VAL_INT;

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

static const unsigned long icm42688_scan_masks[] = {
	GENMASK(ICM42688_SCAN_TEMP, ICM42688_SCAN_ACCEL_X),
	0,
};

/* IIO信息结构体 */
static const struct iio_info icm42688_info = {
	.read_raw = icm42688_read_raw,
};

static int icm42688_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct iio_dev *indio_dev = iio_trigger_get_drvdata(trig);
	struct icm42688_data *data = iio_priv(indio_dev);

	return icm42688_update_bits(data, ICM42688_REG_INT_SOURCE0, BIT(3),
				    state ? BIT(3) : 0);
}

static const struct iio_trigger_ops icm42688_trigger_ops = {
	.set_trigger_state = icm42688_set_trigger_state,
	.validate_device = iio_trigger_validate_own_device,
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
	struct icm42688_scan scan = { };
	u8 reg_buf[14];
	int ret;

	/* TEMP、ACCEL、GYRO 在 bank 0 中连续，单次 burst 保证同一采样时刻。 */
	ret = icm42688_read_regs(data, ICM42688_REG_TEMP_DATA1,
				 reg_buf, sizeof(reg_buf));
	if (ret)
		goto done;

	/* 保留传感器大端字节序，与 channel scan_type=IIO_BE 一致。 */
	memcpy(&scan.channels[ICM42688_SCAN_ACCEL_X], &reg_buf[2], 6);
	memcpy(&scan.channels[ICM42688_SCAN_GYRO_X], &reg_buf[8], 6);
	memcpy(&scan.channels[ICM42688_SCAN_TEMP], &reg_buf[0], 2);

	/* 推送到IIO buffer（包含时间戳） */
	iio_push_to_buffers_with_timestamp(indio_dev, &scan, pf->timestamp);

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

	/* INT1: 推挽、低电平有效、脉冲模式，与 falling-edge DT 一致。 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_CONFIG, BIT(1));
	if (ret)
		return ret;

	/* 清除复位默认的异步中断复位位，保证 INT1/INT2 正常工作。 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_CONFIG1, 0x00);
	if (ret)
		return ret;

	/* DRDY 由 IIO trigger set_state 在 buffer enable/disable 时控制。 */
	ret = icm42688_write_reg(data, ICM42688_REG_INT_SOURCE0, 0x00);
	if (ret)
		return ret;

	/* TEMP_DIS 保持 0；启动 ACCEL/GYRO 低噪声模式。 */
	ret = icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				 ICM42688_PWR_ACCEL_MODE_LN |
				 ICM42688_PWR_GYRO_MODE_LN);
	if (ret) {
		dev_err(&data->spi->dev, "启动低噪声模式失败: %d\n", ret);
		return ret;
	}

	/* 等待传感器稳定 */
	msleep(50);

	return 0;
}

static void icm42688_power_off(void *arg)
{
	struct icm42688_data *data = arg;

	(void)icm42688_update_bits(data, ICM42688_REG_INT_SOURCE0, BIT(3), 0);
	(void)icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				 ICM42688_PWR_TEMP_DIS);
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

	/* Mode 0/3 均由器件支持；尊重 DT，拒绝不兼容或超规格配置。 */
	if ((spi->mode & (SPI_CPOL | SPI_CPHA)) != SPI_MODE_0 &&
	    (spi->mode & (SPI_CPOL | SPI_CPHA)) != SPI_MODE_3) {
		dev_err(dev, "只支持 SPI mode 0 或 mode 3\n");
		return -EINVAL;
	}
	if (!spi->max_speed_hz || spi->max_speed_hz > 24000000) {
		dev_err(dev, "无效 SPI 频率: %uHz（最大 24MHz）\n",
			spi->max_speed_hz);
		return -EINVAL;
	}
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

	/* 配置IIO设备 */
	indio_dev->dev.parent = dev;
	indio_dev->name = "icm42688";
	indio_dev->info = &icm42688_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = icm42688_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42688_channels);
	indio_dev->available_scan_masks = icm42688_scan_masks;

	if (spi->irq < 0)
		return dev_err_probe(dev, spi->irq,
				     "data-ready IRQ尚未就绪\n");
	if (spi->irq == 0) {
		dev_err(dev, "缺少 data-ready IRQ；本驱动不提供伪轮询模式\n");
		return -EINVAL;
	}
	data->irq = spi->irq;

	/* 初始化芯片 */
	ret = icm42688_init_chip(data);
	if (ret) {
		dev_err(dev, "芯片初始化失败: %d\n", ret);
		return ret;
	}
	ret = devm_add_action_or_reset(dev, icm42688_power_off, data);
	if (ret)
		return ret;

	data->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
					    indio_dev->name,
					    iio_device_id(indio_dev));
	if (!data->trig)
		return -ENOMEM;
	data->trig->dev.parent = dev;
	data->trig->ops = &icm42688_trigger_ops;
	iio_trigger_set_drvdata(data->trig, indio_dev);

	ret = devm_iio_trigger_register(dev, data->trig);
	if (ret) {
		dev_err(dev, "IIO trigger 注册失败: %d\n", ret);
		return ret;
	}
	ret = devm_request_irq(dev, spi->irq,
			       iio_trigger_generic_data_rdy_poll, 0,
			       data->trig->name, data->trig);
	if (ret) {
		dev_err(dev, "data-ready IRQ 请求失败: %d\n", ret);
		return ret;
	}
	ret = iio_trigger_set_immutable(indio_dev, data->trig);
	if (ret) {
		dev_err(dev, "绑定 immutable trigger 失败: %d\n", ret);
		return ret;
	}

	/* 设置triggered buffer */
	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      icm42688_trigger_handler,
					      NULL);
	if (ret) {
		dev_err(dev, "triggered buffer设置失败: %d\n", ret);
		return ret;
	}

	/* 注册IIO设备 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(dev, "IIO设备注册失败: %d\n", ret);
		return ret;
	}

	dev_info(dev, "ICM42688六轴IMU驱动加载成功 (SPI %dMHz, irq=%d)\n",
		 spi->max_speed_hz / 1000000, data->irq);

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

	return icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				  ICM42688_PWR_TEMP_DIS);
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
	int ret;

	ret = icm42688_write_reg(data, ICM42688_REG_PWR_MGMT0,
				 ICM42688_PWR_ACCEL_MODE_LN |
				 ICM42688_PWR_GYRO_MODE_LN);
	if (!ret)
		msleep(50);
	return ret;
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
	.id_table	= icm42688_id,
};

module_spi_driver(icm42688_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("ICM42688六轴IMU SPI IIO驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
