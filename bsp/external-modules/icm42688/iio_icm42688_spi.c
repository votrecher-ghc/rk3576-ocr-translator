// SPDX-License-Identifier: GPL-2.0
/**
 * @file iio_icm42688_spi.c
 * ICM42688 六轴驱动：SPI 读取加速度、角速度和芯片温度，提供直接读取及 IIO 触发缓冲。
 * 数据就绪中断触发整组寄存器读取，扫描帧附加时间戳；用户态按 scan_elements 解码布局。
 */
/*
 * ICM-42688-P 6-axis IMU SPI IIO driver
 *
 * All register accesses are implemented directly with spi_sync_transfer().
 * The driver exposes accelerometer, gyroscope and die-temperature channels in
 * direct mode and through an IIO triggered buffer.
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/byteorder/generic.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/spi/spi.h>
#include <linux/string.h>

#define ICM42688_DRV_NAME                     "icm42688"
#define ICM42688_SPI_MAX_HZ                   24000000
#define ICM42688_WHO_AM_I_VALUE               0x47

/* ICM42688 SPI command bit. Bit 7 selects a read operation. */
#define ICM42688_SPI_READ                     BIT(7)
#define ICM42688_SPI_REG_MASK                 GENMASK(6, 0)

/* User bank 0 registers. */
#define ICM42688_REG_DEVICE_CONFIG            0x11
#define ICM42688_REG_INT_CONFIG               0x14
#define ICM42688_REG_TEMP_DATA1               0x1d
#define ICM42688_REG_ACCEL_DATA_X1            0x1f
#define ICM42688_REG_ACCEL_DATA_Y1            0x21
#define ICM42688_REG_ACCEL_DATA_Z1            0x23
#define ICM42688_REG_GYRO_DATA_X1             0x25
#define ICM42688_REG_GYRO_DATA_Y1             0x27
#define ICM42688_REG_GYRO_DATA_Z1             0x29
#define ICM42688_REG_INTF_CONFIG0             0x4c
#define ICM42688_REG_PWR_MGMT0                0x4e
#define ICM42688_REG_GYRO_CONFIG0             0x4f
#define ICM42688_REG_ACCEL_CONFIG0            0x50
#define ICM42688_REG_INT_CONFIG1              0x64
#define ICM42688_REG_INT_SOURCE0              0x65
#define ICM42688_REG_WHO_AM_I                 0x75

#define ICM42688_DEVICE_CONFIG_SOFT_RESET      BIT(0)

#define ICM42688_INT1_PUSH_PULL                BIT(1)
#define ICM42688_INT1_ACTIVE_HIGH              BIT(0)

#define ICM42688_INTF_CONFIG0_UI_SIFS_MASK     GENMASK(1, 0)
#define ICM42688_INTF_CONFIG0_DISABLE_I2C      \
	FIELD_PREP(ICM42688_INTF_CONFIG0_UI_SIFS_MASK, 3)

#define ICM42688_PWR_GYRO_MODE_MASK            GENMASK(3, 2)
#define ICM42688_PWR_ACCEL_MODE_MASK           GENMASK(1, 0)
#define ICM42688_SENSOR_MODE_LOW_NOISE         3
#define ICM42688_PWR_6AXIS_LOW_NOISE           \
	(FIELD_PREP(ICM42688_PWR_GYRO_MODE_MASK, \
		    ICM42688_SENSOR_MODE_LOW_NOISE) | \
	 FIELD_PREP(ICM42688_PWR_ACCEL_MODE_MASK, \
		    ICM42688_SENSOR_MODE_LOW_NOISE))
#define ICM42688_PWR_SENSORS_OFF               0x00

#define ICM42688_CONFIG_FS_MASK                GENMASK(7, 5)
#define ICM42688_CONFIG_ODR_MASK               GENMASK(3, 0)
#define ICM42688_GYRO_FS_2000DPS               0
#define ICM42688_ACCEL_FS_4G                   2
#define ICM42688_ODR_1KHZ                      6
#define ICM42688_GYRO_CONFIG                   \
	(FIELD_PREP(ICM42688_CONFIG_FS_MASK, \
		    ICM42688_GYRO_FS_2000DPS) | \
	 FIELD_PREP(ICM42688_CONFIG_ODR_MASK, ICM42688_ODR_1KHZ))
#define ICM42688_ACCEL_CONFIG                  \
	(FIELD_PREP(ICM42688_CONFIG_FS_MASK, ICM42688_ACCEL_FS_4G) | \
	 FIELD_PREP(ICM42688_CONFIG_ODR_MASK, ICM42688_ODR_1KHZ))

#define ICM42688_INT_CONFIG1_ASYNC_RESET       BIT(4)
#define ICM42688_INT_SOURCE0_DATA_READY        BIT(3)

#define ICM42688_DATA_INVALID                  ((s16)-32768)
#define ICM42688_BURST_DATA_LEN                14
#define ICM42688_RESET_DELAY_US                2000
#define ICM42688_GYRO_STARTUP_DELAY_MS         60

/* Fixed configuration exposed through IIO. */
#define ICM42688_SAMPLE_FREQUENCY_HZ           1000
#define ICM42688_ACCEL_SCALE_NANO              1197101
#define ICM42688_GYRO_SCALE_NANO               1065264
#define ICM42688_TEMP_SCALE_MILLI_INT          7
#define ICM42688_TEMP_SCALE_MILLI_MICRO        548309
#define ICM42688_TEMP_OFFSET_MILLI             25000

struct icm42688_data {
	struct spi_device *spi; /**< 借用 SPI core 设备，提供 SPI 模式、片选及总线传输入口。 */
	struct mutex lock; /**< 串行化直接读、触发采样及电源/中断寄存器更新，SPI 同步操作可睡眠。 */
	struct iio_trigger *trig; /**< devm 创建并注册的本设备 IIO 触发器；有板级数据就绪 IRQ 时建立。 */
	bool drdy_enabled; /**< 最近成功配置的数据就绪中断使能状态，挂起/恢复路径据此处理。 */
};

struct icm42688_scan {
	__be16 accel[3]; /**< x/y/z 加速度原始计数，保持大端 16 位扫描格式，不是已换算 SI 的 float。 */
	__be16 gyro[3]; /**< x/y/z 角速度原始计数，大端 16 位；与 IIO 通道 scan_type/index 对应。 */
	__be16 temp; /**< 芯片温度的大端 16 位原始计数，布局位于六轴数据之后。 */
	s64 timestamp __aligned(8); /**< IIO 推入缓冲时附加的触发时间戳，纳秒；__aligned(8) 保证帧内 8 字节对齐。 */
};

enum icm42688_scan_index {
	ICM42688_SCAN_ACCEL_X,
	ICM42688_SCAN_ACCEL_Y,
	ICM42688_SCAN_ACCEL_Z,
	ICM42688_SCAN_GYRO_X,
	ICM42688_SCAN_GYRO_Y,
	ICM42688_SCAN_GYRO_Z,
	ICM42688_SCAN_TEMP,
	ICM42688_SCAN_TIMESTAMP,
};

/*
 * The caller holds data->lock. The command phase and data phase belong to one
 * SPI message, so chip select remains asserted between the two transfers.
 */
/* 调用者持锁；SPI 命令最高位置读标志，连续读取整段寄存器。 */
static int icm42688_read_regs_locked(struct icm42688_data *data, u8 reg,
				     void *rx_buf, size_t len)
{
	u8 command = (reg & ICM42688_SPI_REG_MASK) | ICM42688_SPI_READ;
	struct spi_transfer transfers[] = {
		{
			.tx_buf = &command,
			.len = sizeof(command),
		},
		{
			.rx_buf = rx_buf,
			.len = len,
		},
	};

	if (!len)
		return -EINVAL;

	/* 两段 transfer 属于同一条 SPI message，命令与读数据之间保持片选。
	 * sync 返回后传输已结束，栈上的 command/transfers 才能安全离开作用域；
	 * 若拆成两个独立 message，片选边界会改变设备解读寄存器命令的方式。 */
	return spi_sync_transfer(data->spi, transfers, ARRAY_SIZE(transfers));
}

static int icm42688_read_reg_locked(struct icm42688_data *data, u8 reg,
				    u8 *value)
{
	return icm42688_read_regs_locked(data, reg, value, sizeof(*value));
}

static int icm42688_write_reg_locked(struct icm42688_data *data, u8 reg,
				     u8 value)
{
	u8 tx_buf[] = {
		reg & ICM42688_SPI_REG_MASK,
		value,
	};
	struct spi_transfer transfer = {
		.tx_buf = tx_buf,
		.len = sizeof(tx_buf),
	};

	return spi_sync_transfer(data->spi, &transfer, 1);
}

static int icm42688_update_bits_locked(struct icm42688_data *data, u8 reg,
				       u8 mask, u8 value)
{
	u8 old_value;
	u8 new_value;
	int ret;

	ret = icm42688_read_reg_locked(data, reg, &old_value);
	if (ret)
		return ret;

	new_value = (old_value & ~mask) | (value & mask);
	if (new_value == old_value)
		return 0;

	return icm42688_write_reg_locked(data, reg, new_value);
}

static int icm42688_read_be16_locked(struct icm42688_data *data, u8 reg,
				     int *value)
{
	u8 raw[2];
	s16 sample;
	int ret;

	ret = icm42688_read_regs_locked(data, reg, raw, sizeof(raw));
	if (ret)
		return ret;

	sample = (s16)(((u16)raw[0] << 8) | raw[1]);
	if (sample == ICM42688_DATA_INVALID)
		return -EAGAIN;

	*value = sample;
	return 0;
}

static int icm42688_read_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int *val, int *val2, long mask)
{
	struct icm42688_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			return ret;

		mutex_lock(&data->lock);
		ret = icm42688_read_be16_locked(data, chan->address, val);
		mutex_unlock(&data->lock);

		iio_device_release_direct_mode(indio_dev);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_ACCEL:
			*val = 0;
			*val2 = ICM42688_ACCEL_SCALE_NANO;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_ANGL_VEL:
			*val = 0;
			*val2 = ICM42688_GYRO_SCALE_NANO;
			return IIO_VAL_INT_PLUS_NANO;
		case IIO_TEMP:
			*val = ICM42688_TEMP_SCALE_MILLI_INT;
			*val2 = ICM42688_TEMP_SCALE_MILLI_MICRO;
			return IIO_VAL_INT_PLUS_MICRO;
		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;

		*val = ICM42688_TEMP_OFFSET_MILLI;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = ICM42688_SAMPLE_FREQUENCY_HZ;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

#define ICM42688_ACCEL_CHAN(_axis, _index, _reg)                    \
	{                                                               \
		.type = IIO_ACCEL,                                        \
		.modified = 1,                                            \
		.channel2 = (_axis),                                      \
		.address = (_reg),                                        \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),             \
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),     \
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),  \
		.scan_index = (_index),                                   \
		.scan_type = {                                            \
			.sign = 's',                                        \
			.realbits = 16,                                     \
			.storagebits = 16,                                  \
			.endianness = IIO_BE,                               \
		},                                                          \
	}

#define ICM42688_GYRO_CHAN(_axis, _index, _reg)                     \
	{                                                               \
		.type = IIO_ANGL_VEL,                                     \
		.modified = 1,                                            \
		.channel2 = (_axis),                                      \
		.address = (_reg),                                        \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),             \
		.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),     \
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),  \
		.scan_index = (_index),                                   \
		.scan_type = {                                            \
			.sign = 's',                                        \
			.realbits = 16,                                     \
			.storagebits = 16,                                  \
			.endianness = IIO_BE,                               \
		},                                                          \
	}

static const struct iio_chan_spec icm42688_channels[] = {
	ICM42688_ACCEL_CHAN(IIO_MOD_X, ICM42688_SCAN_ACCEL_X,
			    ICM42688_REG_ACCEL_DATA_X1),
	ICM42688_ACCEL_CHAN(IIO_MOD_Y, ICM42688_SCAN_ACCEL_Y,
			    ICM42688_REG_ACCEL_DATA_Y1),
	ICM42688_ACCEL_CHAN(IIO_MOD_Z, ICM42688_SCAN_ACCEL_Z,
			    ICM42688_REG_ACCEL_DATA_Z1),
	ICM42688_GYRO_CHAN(IIO_MOD_X, ICM42688_SCAN_GYRO_X,
			   ICM42688_REG_GYRO_DATA_X1),
	ICM42688_GYRO_CHAN(IIO_MOD_Y, ICM42688_SCAN_GYRO_Y,
			   ICM42688_REG_GYRO_DATA_Y1),
	ICM42688_GYRO_CHAN(IIO_MOD_Z, ICM42688_SCAN_GYRO_Z,
			   ICM42688_REG_GYRO_DATA_Z1),
	{
		.type = IIO_TEMP,
		.address = ICM42688_REG_TEMP_DATA1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = ICM42688_SCAN_TEMP,
		.scan_type = {
			.sign = 's',
			.realbits = 16,
			.storagebits = 16,
			.endianness = IIO_BE,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(ICM42688_SCAN_TIMESTAMP),
};

static const unsigned long icm42688_scan_masks[] = {
	GENMASK(ICM42688_SCAN_TEMP, ICM42688_SCAN_ACCEL_X),
	0,
};

static const struct iio_info icm42688_iio_info = {
	.read_raw = icm42688_read_raw,
};

/* 过滤芯片规定的无效采样哨兵值，避免将异常计数送入姿态融合。 */
static bool icm42688_scan_is_valid(const struct icm42688_scan *scan)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(scan->accel); i++)
		if ((s16)be16_to_cpu(scan->accel[i]) == ICM42688_DATA_INVALID)
			return false;

	for (i = 0; i < ARRAY_SIZE(scan->gyro); i++)
		if ((s16)be16_to_cpu(scan->gyro[i]) == ICM42688_DATA_INVALID)
			return false;

	return (s16)be16_to_cpu(scan->temp) != ICM42688_DATA_INVALID;
}

/* 在可睡眠的 IIO 触发处理阶段批量读传感器，重排扫描字段并附上触发时间戳。 */
static irqreturn_t icm42688_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct icm42688_data *data = iio_priv(indio_dev);
	struct icm42688_scan scan = { };
	u8 raw[ICM42688_BURST_DATA_LEN];
	int ret;

	mutex_lock(&data->lock);
	ret = icm42688_read_regs_locked(data, ICM42688_REG_TEMP_DATA1,
					 raw, sizeof(raw));
	mutex_unlock(&data->lock);
	if (ret)
		goto out_done;

	/* 寄存器顺序为 temp、accel、gyro，而 IIO 扫描顺序由通道 index 定义。
	 * 分别搬到 scan 对应字段，同时保留寄存器的大端字节序供 scan_type 描述；
	 * 不能把原始 burst 字节直接当成用户态的 imu_sample_t 发送。 */
	memcpy(&scan.temp, &raw[0], sizeof(scan.temp));
	memcpy(scan.accel, &raw[2], sizeof(scan.accel));
	memcpy(scan.gyro, &raw[8], sizeof(scan.gyro));

	if (icm42688_scan_is_valid(&scan))
		iio_push_to_buffers_with_timestamp(indio_dev, &scan,
					   pf->timestamp);

out_done:
	/* 包括 SPI 失败/采样无效在内，每次触发处理结束都必须通知 IIO，
	 * 否则触发器可能持续认为上次消费未完成，后续中断不再正常推进缓冲。 */
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

/* 随 IIO buffer 启停打开或关闭芯片数据就绪中断，并记录实际状态。 */
static int icm42688_set_trigger_state(struct iio_trigger *trig, bool state)
{
	struct icm42688_data *data = iio_trigger_get_drvdata(trig);
	int ret;

	mutex_lock(&data->lock);
	ret = icm42688_update_bits_locked(data, ICM42688_REG_INT_SOURCE0,
					  ICM42688_INT_SOURCE0_DATA_READY,
					  state ?
					  ICM42688_INT_SOURCE0_DATA_READY : 0);
	if (!ret)
		data->drdy_enabled = state;
	mutex_unlock(&data->lock);

	return ret;
}

static const struct iio_trigger_ops icm42688_trigger_ops = {
	.set_trigger_state = icm42688_set_trigger_state,
	.validate_device = iio_trigger_validate_own_device,
};

static int icm42688_chip_init(struct icm42688_data *data)
{
	u8 who_am_i;
	int ret;

	mutex_lock(&data->lock);

	ret = icm42688_write_reg_locked(data, ICM42688_REG_DEVICE_CONFIG,
					ICM42688_DEVICE_CONFIG_SOFT_RESET);
	if (ret)
		goto out_unlock;

	usleep_range(ICM42688_RESET_DELAY_US,
		     ICM42688_RESET_DELAY_US + 1000);

	ret = icm42688_read_reg_locked(data, ICM42688_REG_WHO_AM_I,
				       &who_am_i);
	if (ret)
		goto out_unlock;

	if (who_am_i != ICM42688_WHO_AM_I_VALUE) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = icm42688_update_bits_locked(data, ICM42688_REG_INTF_CONFIG0,
					  ICM42688_INTF_CONFIG0_UI_SIFS_MASK,
					  ICM42688_INTF_CONFIG0_DISABLE_I2C);
	if (ret)
		goto out_unlock;

	ret = icm42688_write_reg_locked(data, ICM42688_REG_GYRO_CONFIG0,
					ICM42688_GYRO_CONFIG);
	if (ret)
		goto out_unlock;

	ret = icm42688_write_reg_locked(data, ICM42688_REG_ACCEL_CONFIG0,
					ICM42688_ACCEL_CONFIG);
	if (ret)
		goto out_unlock;

	ret = icm42688_update_bits_locked(data, ICM42688_REG_INT_CONFIG1,
					  ICM42688_INT_CONFIG1_ASYNC_RESET, 0);
	if (ret)
		goto out_unlock;

	ret = icm42688_update_bits_locked(data, ICM42688_REG_INT_SOURCE0,
					  ICM42688_INT_SOURCE0_DATA_READY, 0);
	if (ret)
		goto out_unlock;

	ret = icm42688_write_reg_locked(data, ICM42688_REG_INT_CONFIG,
					ICM42688_INT1_PUSH_PULL |
					ICM42688_INT1_ACTIVE_HIGH);
	if (ret)
		goto out_unlock;

	ret = icm42688_write_reg_locked(data, ICM42688_REG_PWR_MGMT0,
					ICM42688_PWR_6AXIS_LOW_NOISE);

out_unlock:
	mutex_unlock(&data->lock);

	if (!ret)
		msleep(ICM42688_GYRO_STARTUP_DELAY_MS);

	return ret;
}

static void icm42688_power_down(void *private)
{
	struct icm42688_data *data = private;

	mutex_lock(&data->lock);
	icm42688_update_bits_locked(data, ICM42688_REG_INT_SOURCE0,
				    ICM42688_INT_SOURCE0_DATA_READY, 0);
	icm42688_write_reg_locked(data, ICM42688_REG_PWR_MGMT0,
				  ICM42688_PWR_SENSORS_OFF);
	mutex_unlock(&data->lock);
}

/* 把板级 IRQ 接入 IIO 自有触发器；未配置 IRQ 时不建立数据就绪触发。 */
static int icm42688_setup_irq_trigger(struct device *dev,
				      struct iio_dev *indio_dev)
{
	struct icm42688_data *data = iio_priv(indio_dev);
	int ret;

	if (data->spi->irq <= 0)
		return 0;

	data->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
					    indio_dev->name,
					    iio_device_id(indio_dev));
	if (!data->trig)
		return -ENOMEM;

	data->trig->ops = &icm42688_trigger_ops;
	data->trig->dev.parent = dev;
	iio_trigger_set_drvdata(data->trig, data);

	ret = devm_iio_trigger_register(dev, data->trig);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, data->spi->irq,
			       iio_trigger_generic_data_rdy_poll,
			       IRQF_TRIGGER_RISING,
			       dev_name(dev), data->trig);
	if (ret)
		return ret;

	return iio_trigger_set_immutable(indio_dev, data->trig);
}

/* 配置 SPI、初始化芯片、注册触发缓冲及 IIO 设备；devm 在失败时逆序释放资源。 */
static int icm42688_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct icm42688_data *data;
	struct iio_dev *indio_dev;
	int ret;

	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	if (!spi->max_speed_hz || spi->max_speed_hz > ICM42688_SPI_MAX_HZ)
		spi->max_speed_hz = ICM42688_SPI_MAX_HZ;

	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "failed to configure SPI\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->spi = spi;
	mutex_init(&data->lock);

	spi_set_drvdata(spi, indio_dev);

	ret = icm42688_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialise ICM-42688-P\n");

	ret = devm_add_action_or_reset(dev, icm42688_power_down, data);
	if (ret)
		return ret;

	indio_dev->name = ICM42688_DRV_NAME;
	indio_dev->info = &icm42688_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = icm42688_channels;
	indio_dev->num_channels = ARRAY_SIZE(icm42688_channels);
	indio_dev->available_scan_masks = icm42688_scan_masks;

	ret = icm42688_setup_irq_trigger(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to configure data-ready trigger\n");

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      icm42688_trigger_handler,
					      NULL);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to configure triggered buffer\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");

	dev_info(dev,
		 "ICM-42688-P registered at %u Hz using direct SPI transfers\n",
		 ICM42688_SAMPLE_FREQUENCY_HZ);

	return 0;
}

static void icm42688_remove(struct spi_device *spi)
{
	/* Managed cleanup powers the sensor down. */
}

static int __maybe_unused icm42688_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm42688_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = icm42688_update_bits_locked(data, ICM42688_REG_INT_SOURCE0,
					  ICM42688_INT_SOURCE0_DATA_READY, 0);
	if (!ret)
		ret = icm42688_write_reg_locked(data, ICM42688_REG_PWR_MGMT0,
						  ICM42688_PWR_SENSORS_OFF);
	mutex_unlock(&data->lock);

	return ret;
}

static int __maybe_unused icm42688_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct icm42688_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = icm42688_write_reg_locked(data, ICM42688_REG_PWR_MGMT0,
					ICM42688_PWR_6AXIS_LOW_NOISE);
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	msleep(ICM42688_GYRO_STARTUP_DELAY_MS);

	if (data->drdy_enabled) {
		mutex_lock(&data->lock);
		ret = icm42688_update_bits_locked(
			data, ICM42688_REG_INT_SOURCE0,
			ICM42688_INT_SOURCE0_DATA_READY,
			ICM42688_INT_SOURCE0_DATA_READY);
		mutex_unlock(&data->lock);
	}

	return ret;
}

static SIMPLE_DEV_PM_OPS(icm42688_pm_ops,
			  icm42688_suspend, icm42688_resume);

static const struct of_device_id icm42688_of_match[] = {
	{ .compatible = "invensense,icm42688" },
	{ .compatible = "ocr,icm42688" },
	{ }
};
MODULE_DEVICE_TABLE(of, icm42688_of_match);

static const struct spi_device_id icm42688_spi_ids[] = {
	{ "icm42688", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, icm42688_spi_ids);

static struct spi_driver icm42688_driver = {
	.driver = {
		.name = ICM42688_DRV_NAME,
		.of_match_table = icm42688_of_match,
		.pm = &icm42688_pm_ops,
	},
	.probe = icm42688_probe,
	.remove = icm42688_remove,
	.id_table = icm42688_spi_ids,
};
module_spi_driver(icm42688_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("ICM-42688-P direct-transfer SPI 6-axis IMU IIO driver");
MODULE_LICENSE("GPL");
