// SPDX-License-Identifier: GPL-2.0
/*
 * AP3216C ambient-light, proximity and infrared sensor driver
 *
 * The AP3216C is connected through I2C and exposes three direct-mode
 * Industrial I/O channels:
 *
 *   in_illuminance_raw
 *   in_illuminance_scale
 *   in_proximity_raw
 *   in_intensity_ir_raw
 *
 * Interrupt/event support is intentionally not included in this first
 * version. The INT pin may remain unconnected and userspace can read the
 * direct-mode IIO attributes.
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
#include <linux/regmap.h>

#define AP3216C_DRV_NAME                     "ap3216c"

/* System registers. */
#define AP3216C_REG_SYSTEM_CONFIG             0x00
#define AP3216C_REG_INT_STATUS                0x01
#define AP3216C_REG_INT_CLEAR_MANNER          0x02
#define AP3216C_REG_IR_DATA_LOW               0x0a
#define AP3216C_REG_IR_DATA_HIGH              0x0b
#define AP3216C_REG_ALS_DATA_LOW              0x0c
#define AP3216C_REG_ALS_DATA_HIGH             0x0d
#define AP3216C_REG_PS_DATA_LOW               0x0e
#define AP3216C_REG_PS_DATA_HIGH              0x0f

/* ALS registers. */
#define AP3216C_REG_ALS_CONFIG                 0x10
#define AP3216C_REG_ALS_CALIBRATION            0x19

/* PS/IR registers. */
#define AP3216C_REG_PS_CONFIG                  0x20
#define AP3216C_REG_PS_LED_CONTROL             0x21
#define AP3216C_REG_PS_INT_MODE                0x22
#define AP3216C_REG_PS_MEAN_TIME               0x23
#define AP3216C_REG_PS_LED_WAITING             0x24
#define AP3216C_REG_MAX                        0x2d

/* SYSTEM_CONFIG[2:0]. */
#define AP3216C_MODE_POWER_DOWN                0x00
#define AP3216C_MODE_ALS_ACTIVE                0x01
#define AP3216C_MODE_PS_IR_ACTIVE              0x02
#define AP3216C_MODE_ALS_PS_IR_ACTIVE          0x03
#define AP3216C_MODE_SW_RESET                  0x04

/* Data field masks. */
#define AP3216C_IR_OVERFLOW                    BIT(7)
#define AP3216C_IR_DATA_LOW_MASK               GENMASK(1, 0)

#define AP3216C_PS_OBJECT_NEAR                 BIT(7)
#define AP3216C_PS_IR_OVERFLOW                 BIT(6)
#define AP3216C_PS_DATA_LOW_MASK               GENMASK(3, 0)
#define AP3216C_PS_DATA_HIGH_MASK              GENMASK(5, 0)

#define AP3216C_ALS_RANGE_SHIFT                4
#define AP3216C_ALS_RANGE_MASK                 GENMASK(5, 4)

/* Datasheet reset time is 10 ms. */
#define AP3216C_RESET_DELAY_US_MIN             10000
#define AP3216C_RESET_DELAY_US_MAX             12000

/* Combined ALS + PS/IR conversion time is typically 112.5 ms. */
#define AP3216C_FIRST_SAMPLE_DELAY_MS          120

/* Explicit datasheet defaults used by this driver. */
#define AP3216C_ALS_CONFIG_DEFAULT             0x00
#define AP3216C_PS_CONFIG_DEFAULT              0x05
#define AP3216C_PS_LED_CONTROL_DEFAULT         0x13
#define AP3216C_INT_CLEAR_AUTOMATIC            0x00

struct ap3216c_data {
	struct regmap *regmap;
	struct mutex lock;
};

/* Resolution in micro-lux per ADC count for ALS_CONFIG[5:4]. */
static const int ap3216c_als_scale_micro[] = {
	350000,
	78800,
	19700,
	4900,
};

static const struct regmap_config ap3216c_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AP3216C_REG_MAX,
	.cache_type = REGCACHE_NONE,
};

/*
 * AP3216C latches the high byte when the corresponding low byte is read.
 * Keep the reads ordered and protected by the device mutex.
 */
static int ap3216c_read_pair_locked(struct ap3216c_data *data,
				    unsigned int low_reg,
				    unsigned int high_reg,
				    unsigned int *low,
				    unsigned int *high)
{
	int ret;

	ret = regmap_read(data->regmap, low_reg, low);
	if (ret)
		return ret;

	return regmap_read(data->regmap, high_reg, high);
}

static int ap3216c_read_als_locked(struct ap3216c_data *data, int *value)
{
	unsigned int low;
	unsigned int high;
	int ret;

	ret = ap3216c_read_pair_locked(data,
					AP3216C_REG_ALS_DATA_LOW,
					AP3216C_REG_ALS_DATA_HIGH,
					&low, &high);
	if (ret)
		return ret;

	*value = ((high & 0xff) << 8) | (low & 0xff);
	return 0;
}

static int ap3216c_read_ir_locked(struct ap3216c_data *data, int *value)
{
	unsigned int low;
	unsigned int high;
	int ret;

	ret = ap3216c_read_pair_locked(data,
					AP3216C_REG_IR_DATA_LOW,
					AP3216C_REG_IR_DATA_HIGH,
					&low, &high);
	if (ret)
		return ret;

	if (low & AP3216C_IR_OVERFLOW)
		return -EOVERFLOW;

	*value = ((high & 0xff) << 2) |
		 (low & AP3216C_IR_DATA_LOW_MASK);
	return 0;
}

static int ap3216c_read_ps_locked(struct ap3216c_data *data, int *value)
{
	unsigned int low;
	unsigned int high;
	int ret;

	ret = ap3216c_read_pair_locked(data,
					AP3216C_REG_PS_DATA_LOW,
					AP3216C_REG_PS_DATA_HIGH,
					&low, &high);
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
	unsigned int config;
	unsigned int range;
	int ret;

	ret = regmap_read(data->regmap, AP3216C_REG_ALS_CONFIG, &config);
	if (ret)
		return ret;

	range = (config & AP3216C_ALS_RANGE_MASK) >>
		AP3216C_ALS_RANGE_SHIFT;

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
		if (chan->type != IIO_LIGHT) {
			ret = -EINVAL;
			break;
		}

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

static int ap3216c_set_mode_locked(struct ap3216c_data *data,
				   unsigned int mode)
{
	return regmap_write(data->regmap, AP3216C_REG_SYSTEM_CONFIG,
			    mode);
}

static int ap3216c_chip_init(struct ap3216c_data *data)
{
	unsigned int config;
	int ret;

	mutex_lock(&data->lock);

	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_SW_RESET);
	if (ret)
		goto out_unlock;

	usleep_range(AP3216C_RESET_DELAY_US_MIN,
		     AP3216C_RESET_DELAY_US_MAX);

	ret = regmap_write(data->regmap, AP3216C_REG_INT_CLEAR_MANNER,
			   AP3216C_INT_CLEAR_AUTOMATIC);
	if (ret)
		goto out_unlock;

	ret = regmap_write(data->regmap, AP3216C_REG_ALS_CONFIG,
			   AP3216C_ALS_CONFIG_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = regmap_write(data->regmap, AP3216C_REG_PS_CONFIG,
			   AP3216C_PS_CONFIG_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = regmap_write(data->regmap, AP3216C_REG_PS_LED_CONTROL,
			   AP3216C_PS_LED_CONTROL_DEFAULT);
	if (ret)
		goto out_unlock;

	ret = ap3216c_set_mode_locked(data,
				      AP3216C_MODE_ALS_PS_IR_ACTIVE);
	if (ret)
		goto out_unlock;

	/* A readback catches a missing or non-responsive device. */
	ret = regmap_read(data->regmap, AP3216C_REG_SYSTEM_CONFIG, &config);
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

static int ap3216c_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ap3216c_data *data;
	struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	mutex_init(&data->lock);

	data->regmap = devm_regmap_init_i2c(client, &ap3216c_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap),
				     "failed to initialise regmap\n");

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

static int ap3216c_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	ret = ap3216c_set_mode_locked(data, AP3216C_MODE_POWER_DOWN);
	mutex_unlock(&data->lock);

	return ret;
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
	ret = ap3216c_set_mode_locked(data,
				      AP3216C_MODE_ALS_PS_IR_ACTIVE);
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
	.probe = ap3216c_probe,
	.remove = ap3216c_remove,
	.id_table = ap3216c_i2c_ids,
};
module_i2c_driver(ap3216c_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("AP3216C ambient-light, proximity and IR IIO driver");
MODULE_LICENSE("GPL");
