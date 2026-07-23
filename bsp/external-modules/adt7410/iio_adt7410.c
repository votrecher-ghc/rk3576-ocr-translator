// SPDX-License-Identifier: GPL-2.0
/*
 * ADT7410 high-accuracy digital temperature sensor driver
 *
 * All register accesses are implemented directly with i2c_transfer().
 * The device is exposed as one direct-mode Industrial I/O temperature
 * channel and configured for 16-bit continuous conversion.
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

#define ADT7410_DRV_NAME                      "adt7410"

/* Register map. Multi-byte temperature registers are stored MSB first. */
#define ADT7410_REG_TEMPERATURE               0x00
#define ADT7410_REG_STATUS                    0x02
#define ADT7410_REG_CONFIG                    0x03
#define ADT7410_REG_ID                        0x0b

/* STATUS register. */
#define ADT7410_STATUS_NOT_READY              BIT(7)

/* CONFIG register. */
#define ADT7410_CONFIG_CT_POLARITY            BIT(2)
#define ADT7410_CONFIG_INT_POLARITY           BIT(3)
#define ADT7410_CONFIG_EVENT_MODE             BIT(4)
#define ADT7410_CONFIG_MODE_MASK              GENMASK(6, 5)
#define ADT7410_CONFIG_MODE_CONTINUOUS        0
#define ADT7410_CONFIG_MODE_SHUTDOWN          GENMASK(6, 5)
#define ADT7410_CONFIG_RESOLUTION_16BIT       BIT(7)

/* ID[7:4] identifies the ADT7410; ID[3:0] is the silicon revision. */
#define ADT7410_ID_MASK                       GENMASK(7, 4)
#define ADT7410_ID_VALUE                      0xc0

/*
 * IIO temperature scale is expressed in millidegrees Celsius per raw count.
 * In 16-bit mode one LSB is 1/128 degrees Celsius = 7.8125 millidegrees.
 */
#define ADT7410_SCALE_MILLI_INT               7
#define ADT7410_SCALE_MILLI_MICRO             812500

#define ADT7410_READY_RETRIES                 6
#define ADT7410_READY_RETRY_DELAY_MS          60

struct adt7410_data {
	struct i2c_client *client;
	struct mutex lock;
	u8 original_config;
	u8 active_config;
};

/*
 * The caller serializes access with data->lock. Reads use a write message for
 * the register address followed by a repeated-start read message.
 */
static int adt7410_read_reg_locked(struct adt7410_data *data, u8 reg, u8 *value)
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

static int adt7410_write_reg_locked(struct adt7410_data *data,
				    u8 reg, u8 value)
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

static int adt7410_read_be16_locked(struct adt7410_data *data,
				    u8 reg, u16 *value)
{
	struct i2c_client *client = data->client;
	u16 flags = client->flags & I2C_M_TEN;
	u8 reg_buf = reg;
	u8 rx_buf[2];
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
			.len = sizeof(rx_buf),
			.buf = rx_buf,
		},
	};
	int ret;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*value = ((u16)rx_buf[0] << 8) | rx_buf[1];
	return 0;
}

static int adt7410_wait_ready_locked(struct adt7410_data *data)
{
	u8 status;
	int ret;
	int i;

	for (i = 0; i < ADT7410_READY_RETRIES; i++) {
		ret = adt7410_read_reg_locked(data, ADT7410_REG_STATUS, &status);
		if (ret)
			return ret;

		if (!(status & ADT7410_STATUS_NOT_READY))
			return 0;

		msleep(ADT7410_READY_RETRY_DELAY_MS);
	}

	return -ETIMEDOUT;
}

static int adt7410_read_temperature_locked(struct adt7410_data *data,
					   int *value)
{
	u16 raw;
	int ret;

	ret = adt7410_wait_ready_locked(data);
	if (ret)
		return ret;

	ret = adt7410_read_be16_locked(data, ADT7410_REG_TEMPERATURE, &raw);
	if (ret)
		return ret;

	*value = (s16)raw;
	return 0;
}

static int adt7410_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct adt7410_data *data = iio_priv(indio_dev);
	int ret;

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		ret = adt7410_read_temperature_locked(data, val);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = ADT7410_SCALE_MILLI_INT;
		*val2 = ADT7410_SCALE_MILLI_MICRO;
		return IIO_VAL_INT_PLUS_MICRO;

	default:
		return -EINVAL;
	}
}

static const struct iio_info adt7410_iio_info = {
	.read_raw = adt7410_read_raw,
};

static const struct iio_chan_spec adt7410_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int adt7410_write_config_locked(struct adt7410_data *data, u8 config)
{
	return adt7410_write_reg_locked(data, ADT7410_REG_CONFIG, config);
}

static void adt7410_restore_config(void *private)
{
	struct adt7410_data *data = private;

	mutex_lock(&data->lock);
	adt7410_write_config_locked(data, data->original_config);
	mutex_unlock(&data->lock);
}

static int adt7410_chip_init(struct adt7410_data *data)
{
	u8 value;
	int ret;

	mutex_lock(&data->lock);

	ret = adt7410_read_reg_locked(data, ADT7410_REG_ID, &value);
	if (ret)
		goto out_unlock;

	if ((value & ADT7410_ID_MASK) != ADT7410_ID_VALUE) {
		ret = -ENODEV;
		goto out_unlock;
	}

	ret = adt7410_read_reg_locked(data, ADT7410_REG_CONFIG, &value);
	if (ret)
		goto out_unlock;

	data->original_config = value;
	data->active_config = data->original_config;
	data->active_config &= ~(ADT7410_CONFIG_MODE_MASK |
				 ADT7410_CONFIG_CT_POLARITY |
				 ADT7410_CONFIG_INT_POLARITY);
	data->active_config |= ADT7410_CONFIG_MODE_CONTINUOUS |
			       ADT7410_CONFIG_RESOLUTION_16BIT |
			       ADT7410_CONFIG_EVENT_MODE;

	ret = adt7410_write_config_locked(data, data->active_config);
	if (ret)
		goto out_unlock;

	ret = adt7410_wait_ready_locked(data);

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

static int adt7410_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct adt7410_data *data;
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

	ret = adt7410_chip_init(data);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialise ADT7410\n");

	ret = devm_add_action_or_reset(dev, adt7410_restore_config, data);
	if (ret)
		return ret;

	indio_dev->name = ADT7410_DRV_NAME;
	indio_dev->info = &adt7410_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adt7410_channels;
	indio_dev->num_channels = ARRAY_SIZE(adt7410_channels);

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register IIO device\n");

	dev_info(dev, "ADT7410 temperature sensor registered\n");
	return 0;
}

static int adt7410_remove(struct i2c_client *client)
{
	return 0;
}

static int __maybe_unused adt7410_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct adt7410_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = adt7410_write_config_locked(data,
					  data->active_config |
					  ADT7410_CONFIG_MODE_SHUTDOWN);
	mutex_unlock(&data->lock);

	return ret;
}

static int __maybe_unused adt7410_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct adt7410_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = adt7410_write_config_locked(data, data->active_config);
	if (!ret)
		ret = adt7410_wait_ready_locked(data);
	mutex_unlock(&data->lock);

	return ret;
}

static SIMPLE_DEV_PM_OPS(adt7410_pm_ops, adt7410_suspend, adt7410_resume);

static const struct of_device_id adt7410_of_match[] = {
	{ .compatible = "adi,adt7410" },
	{ .compatible = "ocr,adt7410" },
	{ }
};
MODULE_DEVICE_TABLE(of, adt7410_of_match);

static const struct i2c_device_id adt7410_i2c_ids[] = {
	{ ADT7410_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, adt7410_i2c_ids);

static struct i2c_driver adt7410_driver = {
	.driver = {
		.name = ADT7410_DRV_NAME,
		.of_match_table = adt7410_of_match,
		.pm = &adt7410_pm_ops,
	},
	.probe = adt7410_probe,
	.remove = adt7410_remove,
	.id_table = adt7410_i2c_ids,
};
module_i2c_driver(adt7410_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("ADT7410 high-accuracy temperature IIO driver");
MODULE_LICENSE("GPL");
