// SPDX-License-Identifier: GPL-2.0
/*
 * iio_adt7410.c - ADT7410温度传感器驱动
 *
 * 描述: 通过I2C读取ADT7410的16位温度数据，使用IIO子系统暴露给用户空间。
 *       I2C地址: 0x48
 *       16bit模式: 分辨率 0.0078°C/LSB
 *
 * 寄存器:
 *   0x00 - TEMP_VALUE_MSB (温度高字节)
 *   0x01 - TEMP_VALUE_LSB (温度低字节)
 *   0x02 - STATUS (状态寄存器)
 *   0x03 - CONFIG (配置寄存器)
 *
 * 作者: OCR Team <ocr@example.com>
 * 许可证: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pm.h>

/* 驱动兼容字符串 */
#define ADT7410_DRV_NAME	"ocr,adt7410"

/* ADT7410 I2C地址 */
#define ADT7410_I2C_ADDR	0x48

/* 寄存器定义 */
#define ADT7410_REG_TEMP_MSB	0x00	/* 温度值高字节 */
#define ADT7410_REG_TEMP_LSB	0x01	/* 温度值低字节 */
#define ADT7410_REG_STATUS	0x02	/* 状态寄存器 */
#define ADT7410_REG_CONFIG	0x03	/* 配置寄存器 */
#define ADT7410_REG_T_HIGH_MSB	0x04	/* 高温阈值高字节 */
#define ADT7410_REG_T_LOW_MSB	0x06	/* 低温阈值高字节 */
#define ADT7410_REG_T_CRIT_MSB	0x08	/* 临界温度阈值高字节 */
#define ADT7410_REG_T_HYST	0x0A	/* 滞后寄存器 */
#define ADT7410_REG_ID		0x0B	/* 器件ID寄存器 */

/* 配置寄存器位定义 */
#define ADT7410_CONFIG_16BIT	0x80	/* 16位分辨率模式 */
#define ADT7410_CONFIG_1SPS	0x40	/* 1 SPS采样率（省电） */
#define ADT7410_CONFIG_INT_MODE	0x20	/* 中断模式（比较器模式=0） */
#define ADT7410_CONFIG_CT_POL	0x10	/* 临界温度输出极性 */
#define ADT7410_CONFIG_INT_POL	0x08	/* 中断输出极性 */

/* 状态寄存器位定义 */
#define ADT7410_STAT_T_LOW	0x40	/* 低于T_LOW */
#define ADT7410_STAT_T_HIGH	0x20	/* 高于T_HIGH */
#define ADT7410_STAT_T_CRIT	0x10	/* 高于T_CRIT */
#define ADT7410_STAT_T_OPEN	0x01	/* 开路故障 */

/* 器件ID: ADT7410的ID寄存器高4位为0xC，低4位为硅片版本 */
#define ADT7410_DEVICE_ID_MASK		0xF0
#define ADT7410_DEVICE_ID_VALUE		0xC0

/* 温度分辨率: 16bit模式 0.0078°C/LSB */
#define ADT7410_TEMP_SCALE_MILLI	7	/* scale * 1000 = 7.8125, 用7表示整数部分 */
#define ADT7410_TEMP_SCALE_NUM		78125	/* scale = 0.0078125 °C/LSB (scale*1e6) */

/**
 * struct adt7410_data - ADT7410设备私有数据
 * @client: I2C客户端指针
 * @lock:   数据访问锁
 */
struct adt7410_data {
	struct i2c_client *client;
	struct mutex lock;
};

/**
 * adt7410_read_temp_raw - 读取原始温度数据
 * @data: 设备私有数据
 *
 * 读取16位温度值（大端序）。
 * 使用i2c_smbus_read_word_swapped读取（I2C设备为大端序，
 * SMBUS word为小端序，swapped函数会自动交换字节）。
 *
 * 返回: 原始温度值（有符号16位），负数错误码
 */
static int adt7410_read_temp_raw(struct adt7410_data *data)
{
	int ret;

	mutex_lock(&data->lock);

	/*
	 * ADT7410温度寄存器为大端序(MSB在前)。
	 * i2c_smbus_read_word_swapped会读取2字节并交换高低字节，
	 * 这样返回的16位值就是大端序的正确值。
	 */
	ret = i2c_smbus_read_word_swapped(data->client, ADT7410_REG_TEMP_MSB);

	mutex_unlock(&data->lock);
	return ret;
}

/**
 * adt7410_read_raw - IIO读取原始数据回调
 * @indio_dev: IIO设备
 * @chan:      IIO通道
 * @val:       返回值第一部分
 * @val2:      返回值第二部分
 * @mask:      读取掩码
 *
 * 返回: 正数为有效值个数，负数为错误码
 */
static int adt7410_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct adt7410_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->type != IIO_TEMP)
			return -EINVAL;

		ret = adt7410_read_temp_raw(data);
		if (ret < 0) {
			dev_err(&data->client->dev, "读取温度失败: %d\n", ret);
			return ret;
		}

		/* 16bit模式: 16位有符号原始值 */
		*val = (s16)ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		if (chan->type != IIO_TEMP)
			return -EINVAL;

		/* 16bit模式: 0.0078125 °C/LSB = 1/128 °C/LSB */
		*val = 0;
		*val2 = 7812; /* 0.0078125, 单位为°C, scale*1e6 = 7812.5 */
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_OFFSET:
		if (chan->type != IIO_TEMP)
			return -EINVAL;

		/* ADT7410 16bit模式: 温度 = raw / 128 (°C) */
		/* 无偏移，直接为 raw * scale */
		*val = 0;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

/**
 * adt7410_write_raw - IIO写入回调（暂不支持写入）
 */
static int adt7410_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	return -EOPNOTSUPP;
}

/* IIO通道定义 */
static const struct iio_chan_spec adt7410_channels[] = {
	{
		.type		= IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET),
		.info_mask_shared_by_all = 0,
		.datasheet_name	= "temp",
	},
};

/* IIO信息结构体 */
static const struct iio_info adt7410_info = {
	.read_raw	= adt7410_read_raw,
	.write_raw	= adt7410_write_raw,
};

/**
 * adt7410_init_chip - 初始化ADT7410芯片
 * @data: 设备私有数据
 *
 * 配置流程:
 * 1. 读取器件ID验证
 * 2. 配置为16bit分辨率模式
 *
 * 返回: 0成功，负数错误码
 */
static int adt7410_init_chip(struct adt7410_data *data)
{
	struct i2c_client *client = data->client;
	int ret;
	int config;
	int id;

	/* 读取器件ID（高4位应为0xC） */
	id = i2c_smbus_read_byte_data(client, ADT7410_REG_ID);
	if (id < 0) {
		dev_err(&client->dev, "读取器件ID失败: %d\n", id);
		return id;
	}

	/* 验证器件ID */
	if ((id & ADT7410_DEVICE_ID_MASK) != ADT7410_DEVICE_ID_VALUE) {
		dev_err(&client->dev,
			"器件ID不匹配: 期望0x%02x, 实际0x%02x\n",
			ADT7410_DEVICE_ID_VALUE, id & ADT7410_DEVICE_ID_MASK);
		return -ENODEV;
	}

	dev_dbg(&client->dev, "ADT7410 器件ID验证通过: 0x%02x\n", id);

	/* 读取当前配置寄存器 */
	config = i2c_smbus_read_byte_data(client, ADT7410_REG_CONFIG);
	if (config < 0) {
		dev_err(&client->dev, "读取配置寄存器失败: %d\n", config);
		return config;
	}

	/* 设置为16bit分辨率模式, 保持其他默认配置 */
	config |= ADT7410_CONFIG_16BIT;

	ret = i2c_smbus_write_byte_data(client, ADT7410_REG_CONFIG, config);
	if (ret) {
		dev_err(&client->dev, "写入配置寄存器失败: %d\n", ret);
		return ret;
	}

	/* 等待第一次转换完成 */
	msleep(240); /* 16bit模式默认240ms完成一次转换 */

	return 0;
}

/**
 * adt7410_probe - I2C驱动探测函数
 * @client: I2C客户端
 * @id:     I2C设备ID
 *
 * 返回: 0成功，负数错误码
 */
static int adt7410_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct adt7410_data *data;
	int ret;

	/* 检查设备存在 */
	ret = i2c_smbus_read_byte_data(client, ADT7410_REG_CONFIG);
	if (ret < 0) {
		dev_err(dev, "无法读取ADT7410设备: %d\n", ret);
		return -ENODEV;
	}

	/* 分配IIO设备 */
	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev) {
		dev_err(dev, "无法分配IIO设备\n");
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	data->client = client;
	mutex_init(&data->lock);

	i2c_set_clientdata(client, indio_dev);

	/* 初始化芯片 */
	ret = adt7410_init_chip(data);
	if (ret) {
		dev_err(dev, "芯片初始化失败: %d\n", ret);
		return ret;
	}

	/* 配置IIO设备 */
	indio_dev->dev.parent = dev;
	indio_dev->name = "adt7410";
	indio_dev->info = &adt7410_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adt7410_channels;
	indio_dev->num_channels = ARRAY_SIZE(adt7410_channels);

	/* 注册IIO设备 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(dev, "IIO设备注册失败: %d\n", ret);
		return ret;
	}

	dev_info(dev, "ADT7410温度传感器驱动加载成功 (16bit模式, 0.0078°C/LSB)\n");

	return 0;
}

/**
 * adt7410_remove - I2C驱动移除函数
 * @client: I2C客户端
 *
 * 返回: 0
 */
static int adt7410_remove(struct i2c_client *client)
{
	int config;

	/* 读取配置并清除16bit模式（恢复默认13bit以省电） */
	config = i2c_smbus_read_byte_data(client, ADT7410_REG_CONFIG);
	if (config >= 0) {
		config &= ~ADT7410_CONFIG_16BIT;
		i2c_smbus_write_byte_data(client, ADT7410_REG_CONFIG, config);
	}

	dev_info(&client->dev, "ADT7410驱动卸载完成\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
/**
 * adt7410_suspend - 休眠回调
 * @dev: 设备指针
 *
 * 设置为1SPS省电模式。
 *
 * 返回: 0成功
 */
static int adt7410_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	int config;

	config = i2c_smbus_read_byte_data(client, ADT7410_REG_CONFIG);
	if (config < 0)
		return config;

	/* 设置1SPS省电模式 */
	config |= ADT7410_CONFIG_1SPS;

	return i2c_smbus_write_byte_data(client, ADT7410_REG_CONFIG, config);
}

/**
 * adt7410_resume - 唤醒回调
 * @dev: 设备指针
 *
 * 恢复正常采样模式。
 *
 * 返回: 0成功
 */
static int adt7410_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	int config;

	config = i2c_smbus_read_byte_data(client, ADT7410_REG_CONFIG);
	if (config < 0)
		return config;

	/* 清除1SPS省电模式 */
	config &= ~ADT7410_CONFIG_1SPS;

	return i2c_smbus_write_byte_data(client, ADT7410_REG_CONFIG, config);
}
#endif

static SIMPLE_DEV_PM_OPS(adt7410_pm_ops, adt7410_suspend, adt7410_resume);

/* 设备树匹配表 */
static const struct of_device_id adt7410_of_match[] = {
	{ .compatible = ADT7410_DRV_NAME, },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(of, adt7410_of_match);

/* I2C设备ID表 */
static const struct i2c_device_id adt7410_id[] = {
	{ "adt7410", 0 },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(i2c, adt7410_id);

/* I2C驱动结构体 */
static struct i2c_driver adt7410_driver = {
	.driver = {
		.name		= "adt7410-ocr",
		.of_match_table	= adt7410_of_match,
		.pm		= &adt7410_pm_ops,
	},
	.probe		= adt7410_probe,
	.remove		= adt7410_remove,
	.id_table	= adt7410_id,
};

module_i2c_driver(adt7410_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("ADT7410温度传感器IIO驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
