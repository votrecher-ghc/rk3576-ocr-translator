// SPDX-License-Identifier: GPL-2.0
/*
 * AP3216C ambient-light, proximity and infrared sensor driver
 *
 * All register accesses are implemented directly with i2c_transfer().
 * The device exposes three direct-mode Industrial I/O channels:
 *
 *   in_illuminance_raw
 *   in_illuminance_scale
 *   in_proximity_raw
 *   in_intensity_ir_raw
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>

#define AP3216C_DRV_NAME                     "ap3216c"

/* System registers. */
#define AP3216C_REG_SYSTEM_CONFIG             0x00
#define AP3216C_REG_INT_CLEAR_MANNER          0x02
#define AP3216C_REG_IR_DATA_LOW               0x0a
#define AP3216C_REG_IR_DATA_HIGH              0x0b
#define AP3216C_REG_ALS_DATA_LOW              0x0c
#define AP3216C_REG_ALS_DATA_HIGH             0x0d
#define AP3216C_REG_PS_DATA_LOW               0x0e
#define AP3216C_REG_PS_DATA_HIGH              0x0f

/* ALS registers. */
#define AP3216C_REG_ALS_CONFIG                 0x10

/* PS/IR registers. */
#define AP3216C_REG_PS_CONFIG                  0x20
#define AP3216C_REG_PS_LED_CONTROL             0x21

/* SYSTEM_CONFIG[2:0]. */
#define AP3216C_MODE_POWER_DOWN                0x00
#define AP3216C_MODE_ALS_PS_IR_ACTIVE          0x03
#define AP3216C_MODE_SW_RESET                  0x04

/* Data field masks. */
#define AP3216C_IR_OVERFLOW                    BIT(7)
#define AP3216C_IR_DATA_LOW_MASK               GENMASK(1, 0)

#define AP3216C_PS_IR_OVERFLOW                 BIT(6)
#define AP3216C_PS_DATA_LOW_MASK               GENMASK(3, 0)
#define AP3216C_PS_DATA_HIGH_MASK              GENMASK(5, 0)

#define AP3216C_ALS_RANGE_SHIFT                4
#define AP3216C_ALS_RANGE_MASK                 GENMASK(5, 4)

#define AP3216C_RESET_DELAY_US_MIN             10000
#define AP3216C_RESET_DELAY_US_MAX             12000
#define AP3216C_FIRST_SAMPLE_DELAY_MS          120

#define AP3216C_ALS_CONFIG_DEFAULT             0x00
#define AP3216C_PS_CONFIG_DEFAULT              0x05
#define AP3216C_PS_LED_CONTROL_DEFAULT         0x13
#define AP3216C_INT_CLEAR_AUTOMATIC            0x00

struct ap3216c_data {
	struct i2c_client *client;
	struct mutex lock;
};

/* Resolution in micro-lux per ADC count for ALS_CONFIG[5:4]. */
static const int ap3216c_als_scale_micro[] = {
	350000,
	78800,
	19700,
	4900,
};

/*
 * The caller serializes register access with data->lock. A register read uses
 * a write message for the register address followed by a repeated-start read.
 */
static int ap3216c_read_reg_locked(struct ap3216c_data *data, u8 reg, u8 *value)
{
	struct i2c_client *client = data->client;
	u16 flags = client->flags & I2C_M_TEN;
	u8 reg_buf = reg;
	struct i2c_msg msgs[] = {
		{
			.addr = client->addr,
			.flags = flags,
			.len = sizeof(reg_buf),
			.buf = &reg_buf,
		},
		{
			.addr = client->addr,
			.flags = flags | I2C_M_RD,
			.len = sizeof(*value),
			.buf = value,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	return 0;
}

static int ap3216c_write_reg_locked(struct ap3216c_data *data, u8 reg, u8 value)
{
	struct i2c_client *client = data->client;
	u16 flags = client->flags & I2C_M_TEN;
	u8 tx_buf[] = { reg, value };
	struct i2c_msg msg = {
		.addr = client->addr,
		.flags = flags,
		.len = sizeof(tx_buf),
		.buf = tx_buf,
	};
	int ret;

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
}

/*
 * AP3216C latches the high byte when the corresponding low byte is read. Keep
 * these two transfers ordered and protected by the device mutex.
 */
static int ap3216c_read_pair_locked(struct ap3216c_data *data,
				    u8 low_reg, u8 high_reg,
				    u8 *low, u8 *high)
{
	int ret;

	ret = ap3216c_read_reg_locked(data, low_reg, low);
	if (ret)
		return ret;

	return ap3216c_read_reg_locked(data, high_reg, high);
}

static int ap3216c_read_als_locked(struct ap3216c_data *data, int *value)
{
	u8 low;
	u8 high;
	int ret;

	ret = ap3216c_read_pair_locked(data, AP3216C_REG_ALS_DATA_LOW,
				       AP3216C_REG_ALS_DATA_HIGH, &low, &high);
	if (ret)
		return ret;

	*value = ((unsigned int)high << 8) | low;
	return 0;
}

static int ap3216c_read_ir_locked(struct ap3216c_data *data, int *value)
{
	u8 low;
	u8 high;
	int ret;

	ret = ap3216c_read_pair_locked(data, AP3216C_REG_IR_DATA_LOW,
				       AP3216C_REG_IR_DATA_HIGH, &low, &high);
	if (ret)
		return ret;

	if (low & AP3216C_IR_OVERFLOW)
		return -EOVERFLOW;

	*value = ((unsigned int)high << 2) |
		 (low & AP3216C_IR_DATA_LOW_MASK);
	return 0;
}

static int ap3216c_read_ps_locked(struct ap3216c_data *data, int *value)
{
	u8 low;
	u8 high;
	int ret;

	ret = ap3216c_read_pair_locked(data, AP3216C_REG_PS_DATA_LOW,
				       AP3216C_REG_PS_DATA_HIGH, &low, &high);
	if (ret)
		return ret;

	if ((low | high) & AP3216C_PS_IR_OVERFLOW)
		return -EOVERFLOW;

	*value = ((high & AP3216C_PS_DATA_HIGH_MASK) << 4) |
		 (low & AP3216C_PS_DATA_LOW_MASK);
	return 0;
}

static int ap3216c_read_als_scale_locked(struct ap3216c_data *data,
					 int *val, int *val2)
{
	u8 config;
	unsigned int range;
	int ret;

	ret = ap3216c_read_reg_locked(data, AP3216C_REG_ALS_CONFIG, &config);
	if (ret)
		return ret;

	range = (config & AP3216C_ALS_RANGE_MASK) >> AP3216C_ALS_RANGE_SHIFT;

	*val = 0;
	*val2 = ap3216c_als_scale_micro[range];
	return IIO_VAL_INT_PLUS_MICRO;
}

static int ap3216c_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_LIGHT:
			ret = ap3216c_read_als_locked(data, val);
			break;
		case IIO_PROXIMITY:
			ret = ap3216c_read_ps_locked(data, val);
			break;
		case IIO_INTENSITY:
			ret = ap3216c_read_ir_locked(data, val);
			break;
		default:
			ret = -EINVAL;
			break;
		}

		if (!ret)
			ret = IIO_VAL_INT;
		break;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type != IIO_LIGHT)
			ret = -EINVAL;
		else
			ret = ap3216c_read_als_scale_locked(data, val, val2);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	mutex_unlock(&data->lock);
	return ret;
}

static const struct iio_info ap3216c_iio_info = {
	.read_raw = ap3216c_read_raw,
};

static const struct iio_chan_spec ap3216c_channels[] = {
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.type = IIO_INTENSITY,
		.modified = 1,
		.channel2 = IIO_MOD_LIGHT_IR,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
};

static int ap3216c_set_mode_locked(struct ap3216c_data *data, u8 mode)
{
	return ap3216c_write_reg_locked(data, AP3216C_REG_SYSTEM_CONFIG, mode);
}

static int ap3216c_chip_init(struct ap3216c_data *data)
{
	u8 config;
	int ret;

	mutex_lock(&data->lock);

	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_SW_RESET);
	if (ret)
		goto out_unlock;

	usleep_range(AP3216C_RESET_DELAY_US_MIN,
		     AP3216C_RESET_DELAY_US_MAX);

	ret = ap3216c_write_reg_locked(data, AP3216C_REG_INT_CLEAR_MANNER,
				       AP3216C_INT_CLEAR_AUTOMATIC);
	if (ret)
		goto out_unlock;

	ret = ap3216c_write_reg_locked(data, AP3216C_REG_ALS_CONFIG,
				       AP3216C_ALS_CONFIG_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = ap3216c_write_reg_locked(data, AP3216C_REG_PS_CONFIG,
				       AP3216C_PS_CONFIG_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = ap3216c_write_reg_locked(data, AP3216C_REG_PS_LED_CONTROL,
				       AP3216C_PS_LED_CONTROL_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_ALS_PS_IR_ACTIVE);
	if (ret)
		goto out_unlock;

	ret = ap3216c_read_reg_locked(data, AP3216C_REG_SYSTEM_CONFIG, &config);
	if (ret)
		goto out_unlock;

	if ((config & GENMASK(2, 0)) != AP3216C_MODE_ALS_PS_IR_ACTIVE)
		ret = -ENODEV;

out_unlock:
	mutex_unlock(&data->lock);

	if (!ret)
		msleep(AP3216C_FIRST_SAMPLE_DELAY_MS);

	return ret;
}

static int ap3216c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ap3216c_data *data;
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "adapter does not support plain I2C transfers\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	i2c_set_clientdata(client, indio_dev);

	ret = ap3216c_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialise AP3216C\n");

	indio_dev->name = AP3216C_DRV_NAME;
	indio_dev->info = &ap3216c_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ap3216c_channels;
	indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");

	dev_info(dev, "AP3216C ALS/PS/IR sensor registered\n");
	return 0;
}

static void ap3216c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_POWER_DOWN);
	mutex_unlock(&data->lock);

	if (ret)
		dev_warn(&client->dev, "failed to power down sensor: %d\n", ret);
}

static int __maybe_unused ap3216c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_POWER_DOWN);
	mutex_unlock(&data->lock);

	return ret;
}

static int __maybe_unused ap3216c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_ALS_PS_IR_ACTIVE);
	mutex_unlock(&data->lock);

	if (!ret)
		msleep(AP3216C_FIRST_SAMPLE_DELAY_MS);

	return ret;
}

static SIMPLE_DEV_PM_OPS(ap3216c_pm_ops,
			  ap3216c_suspend, ap3216c_resume);

static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = "liteon,ap3216c" },
	{ .compatible = "ocr,ap3216c" },
	{ }
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

static const struct i2c_device_id ap3216c_i2c_ids[] = {
	{ AP3216C_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_i2c_ids);

static struct i2c_driver ap3216c_driver = {
	.driver = {
		.name = AP3216C_DRV_NAME,
		.of_match_table = ap3216c_of_match,
		.pm = &ap3216c_pm_ops,
	},
	.probe_new = ap3216c_probe,
	.remove = ap3216c_remove,
	.id_table = ap3216c_i2c_ids,
};
module_i2c_driver(ap3216c_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("AP3216C ambient-light, proximity and IR IIO driver");
MODULE_LICENSE("GPL");
