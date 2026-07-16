// SPDX-License-Identifier: GPL-2.0
/*
 * iio_ap3216c.c - AP3216C环境光/接近传感器驱动
 *
 * 描述: 通过I2C读取AP3216C环境光(ALS)和接近(PS)数据，
 *       使用IIO子系统暴露给用户空间。支持中断模式（可选）。
 *       I2C地址: 0x1E
 *
 * 寄存器:
 *   0x00 - SYS配置寄存器
 *   0x0A - PS数据低字节
 *   0x0B - PS数据高字节
 *   0x0C - ALS数据低字节
 *   0x0D - ALS数据高字节
 *
 * 作者: OCR Team <ocr@example.com>
 * 许可证: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/pm.h>

/* 驱动兼容字符串 */
#define AP3216C_DRV_NAME	"ocr,ap3216c"

/* 寄存器定义 */
#define AP3216C_SYS_CONFIG	0x00	/* 系统配置寄存器 */
#define AP3216C_INT_STATUS	0x01	/* 中断状态寄存器 */
#define AP3216C_INT_CLEAR_MANNER	0x02	/* 中断清除方式寄存器 */
#define AP3216C_IR_DATA_LOW	0x0A	/* IR数据低字节 */
#define AP3216C_IR_DATA_HIGH	0x0B	/* IR数据高字节 */
#define AP3216C_ALS_DATA_LOW	0x0C	/* ALS数据低字节 */
#define AP3216C_ALS_DATA_HIGH	0x0D	/* ALS数据高字节 */
#define AP3216C_PS_DATA_LOW	0x0E	/* PS数据低字节 */
#define AP3216C_PS_DATA_HIGH	0x0F	/* PS数据高字节 */
#define AP3216C_ALS_CFG		0x10	/* ALS配置寄存器 */
#define AP3216C_ALS_CALIBRATION	0x19	/* ALS校准寄存器 */
#define AP3216C_ALS_THD_LOW_L	0x1A	/* ALS低阈值低字节 */
#define AP3216C_ALS_THD_LOW_H	0x1B	/* ALS低阈值高字节 */
#define AP3216C_ALS_THD_HIGH_L	0x1C	/* ALS高阈值低字节 */
#define AP3216C_ALS_THD_HIGH_H	0x1D	/* ALS高阈值高字节 */
#define AP3216C_PS_CFG		0x20	/* PS配置寄存器 */
#define AP3216C_PS_LED		0x21	/* PS LED配置寄存器 */
#define AP3216C_PS_INT_MODE	0x22	/* PS中断算法寄存器 */
#define AP3216C_PS_MEAN_TIME	0x23	/* PS平均时间寄存器 */
#define AP3216C_PS_WAIT_TIME	0x24	/* PS等待时间寄存器 */
#define AP3216C_PS_CALIBRATION_L	0x28	/* PS校准低字节 */
#define AP3216C_PS_CALIBRATION_H	0x29	/* PS校准高字节 */
#define AP3216C_PS_THD_LOW_L	0x2A	/* PS低阈值低字节 */
#define AP3216C_PS_THD_LOW_H	0x2B	/* PS低阈值高字节 */
#define AP3216C_PS_THD_HIGH_L	0x2C	/* PS高阈值低字节 */
#define AP3216C_PS_THD_HIGH_H	0x2D	/* PS高阈值高字节 */

/* 系统配置模式 */
#define AP3216C_MODE_POWERDOWN		0x00	/* 关断模式 */
#define AP3216C_MODE_ALS_ONLY		0x01	/* 仅ALS */
#define AP3216C_MODE_PS_ONLY		0x02	/* PS+IR */
#define AP3216C_MODE_ALS_PS		0x03	/* ALS+PS+IR */
#define AP3216C_MODE_SW_RESET		0x04	/* 软件复位 */
#define AP3216C_MODE_ALS_ONCE		0x05	/* 单次ALS */
#define AP3216C_MODE_PS_ONCE		0x06	/* 单次PS+IR */
#define AP3216C_MODE_ALS_PS_ONCE		0x07	/* 单次ALS+PS+IR */

/* 中断状态与清除方式 */
#define AP3216C_INT_ALS			BIT(0)
#define AP3216C_INT_PS			BIT(1)
#define AP3216C_INT_MASK		(AP3216C_INT_ALS | AP3216C_INT_PS)
#define AP3216C_INT_CLEAR_AUTO		0x00
#define AP3216C_INT_CLEAR_SW		BIT(0)

/* PS数据为10bit：0x0F[5:0] << 4 | 0x0E[3:0]。 */
#define AP3216C_PS_DATA_LOW_MASK	GENMASK(3, 0)
#define AP3216C_PS_DATA_HIGH_MASK	GENMASK(5, 0)
#define AP3216C_PS_DATA_HIGH_SHIFT	4
#define AP3216C_PS_IR_OVERFLOW		BIT(6)

/* Rev0.86寄存器默认值。 */
#define AP3216C_PS_CFG_DEFAULT		0x05
#define AP3216C_PS_LED_DEFAULT		0x13

/* ALS数据16bit（0x0D高8bit，0x0C低8bit） */
#define AP3216C_ALS_DATA_SHIFT		8
/* ALS_CFG=0x00 的默认量程：0.35 lux/LSB（0~20661 lux） */
#define AP3216C_ALS_SCALE_MICRO		350000

/* 驱动启动后等待芯片稳定的延时（ms） */
#define AP3216C_STARTUP_DELAY_MS	100

/**
 * struct ap3216c_data - AP3216C设备私有数据
 * @client:  I2C客户端指针
 * @lock:    数据访问互斥锁
 * @irq:     中断号（0表示不使用中断）
 */
struct ap3216c_data {
	struct i2c_client *client;
	struct mutex lock;
	int irq;
};

/**
 * ap3216c_read_reg - 读取单个寄存器
 * @client: I2C客户端
 * @reg:    寄存器地址
 *
 * 返回: 读取到的值（0-255），负数错误码
 */
static int ap3216c_read_reg(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

/**
 * ap3216c_write_reg - 写入单个寄存器
 * @client: I2C客户端
 * @reg:    寄存器地址
 * @val:    写入值
 *
 * 返回: 0成功，负数错误码
 */
static int ap3216c_write_reg(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

/**
 * ap3216c_read_als - 读取环境光数据
 * @data: 设备私有数据
 *
 * ALS数据为16位：高字节(0x0D) << 8 | 低字节(0x0C)
 *
 * 返回: 环境光原始值（0-65535），负数错误码
 */
static int ap3216c_read_als(struct ap3216c_data *data)
{
	int low, high;
	int ret;

	mutex_lock(&data->lock);

	low = ap3216c_read_reg(data->client, AP3216C_ALS_DATA_LOW);
	if (low < 0) {
		ret = low;
		goto out_unlock;
	}

	high = ap3216c_read_reg(data->client, AP3216C_ALS_DATA_HIGH);
	if (high < 0) {
		ret = high;
		goto out_unlock;
	}

	ret = (high << AP3216C_ALS_DATA_SHIFT) | low;

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/**
 * ap3216c_read_ps - 读取接近数据
 * @data: 设备私有数据
 *
 * PS数据为10位：高字节(0x0F)低6位 << 4 | 低字节(0x0E)低4位。
 * 0x0E/0x0F的bit6为IR_OF；置位表示强红外下PS数据无效。
 *
 * 返回: 接近原始值（0-1023），负数错误码
 */
static int ap3216c_read_ps(struct ap3216c_data *data)
{
	int low, high;
	int ret;

	mutex_lock(&data->lock);

	low = ap3216c_read_reg(data->client, AP3216C_PS_DATA_LOW);
	if (low < 0) {
		ret = low;
		goto out_unlock;
	}

	high = ap3216c_read_reg(data->client, AP3216C_PS_DATA_HIGH);
	if (high < 0) {
		ret = high;
		goto out_unlock;
	}

	if ((low | high) & AP3216C_PS_IR_OVERFLOW) {
		ret = -EOVERFLOW;
		goto out_unlock;
	}

	ret = ((high & AP3216C_PS_DATA_HIGH_MASK) <<
	       AP3216C_PS_DATA_HIGH_SHIFT) |
	      (low & AP3216C_PS_DATA_LOW_MASK);

out_unlock:
	mutex_unlock(&data->lock);
	return ret;
}

/**
 * ap3216c_read_raw - IIO读取原始数据回调
 * @indio_dev: IIO设备
 * @chan:      IIO通道
 * @val:       返回值的第一部分
 * @val2:      返回值的第二部分（用于小数）
 * @mask:      读取掩码（IIO_CHAN_INFO_RAW / IIO_CHAN_INFO_SCALE）
 *
 * 返回:
 *   正数 - val和val2中有效值的个数
 *   负数 - 错误码
 */
static int ap3216c_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct ap3216c_data *data = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		switch (chan->type) {
		case IIO_LIGHT:
			ret = ap3216c_read_als(data);
			if (ret < 0)
				return ret;
			*val = ret;
			return IIO_VAL_INT;

		case IIO_PROXIMITY:
			ret = ap3216c_read_ps(data);
			if (ret < 0)
				return ret;
			*val = ret;
			return IIO_VAL_INT;

		default:
			return -EINVAL;
		}

	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_LIGHT:
			/* IIO_LIGHT scale 单位为 lux/LSB。 */
			*val = 0;
			*val2 = AP3216C_ALS_SCALE_MICRO;
			return IIO_VAL_INT_PLUS_MICRO;

		default:
			return -EINVAL;
		}

	default:
		return -EINVAL;
	}
}

/* IIO通道定义 */
static const struct iio_chan_spec ap3216c_channels[] = {
	{
		.type		= IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.datasheet_name	= "ALS",
	},
	{
		.type		= IIO_PROXIMITY,
		/* PS is a relative 10-bit count with no physical scale. */
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.datasheet_name	= "PS",
	},
};

/* IIO信息结构体 */
static const struct iio_info ap3216c_info = {
	.read_raw	= ap3216c_read_raw,
};

/**
 * ap3216c_irq_handler - 中断处理函数（可选）
 * @irq:    中断号
 * @dev_id: 私有数据（IIO设备指针）
 *
 * 当PS或ALS超出阈值时触发中断，此处仅作为示例唤醒用户空间轮询。
 *
 * 返回: IRQ_HANDLED
 */
static irqreturn_t ap3216c_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ap3216c_data *data = iio_priv(indio_dev);
	int status, ret;

	mutex_lock(&data->lock);
	status = ap3216c_read_reg(data->client, AP3216C_INT_STATUS);
	if (status < 0) {
		dev_err(&data->client->dev, "读取中断状态失败: %d\n", status);
		goto out_unlock;
	}
	status &= AP3216C_INT_MASK;

	dev_dbg(&data->client->dev, "AP3216C中断触发, 状态=0x%02x\n", status);

	/* CLR_MNR=1时，向已置位的状态位写1清除；不要伪造ALS位。 */
	if (status) {
		ret = ap3216c_write_reg(data->client, AP3216C_INT_STATUS, status);
		if (ret)
			dev_err(&data->client->dev,
				"清除中断状态失败: %d\n", ret);
	}

out_unlock:
	mutex_unlock(&data->lock);
	return IRQ_HANDLED;
}

/**
 * ap3216c_init_chip - 初始化AP3216C芯片
 * @data: 设备私有数据
 *
 * 配置流程:
 * 1. 软件复位
 * 2. 等待复位完成
 * 3. 配置ALS和PS参数
 * 4. 启动ALS+PS模式
 *
 * 返回: 0成功，负数错误码
 */
static int ap3216c_init_chip(struct ap3216c_data *data)
{
	struct i2c_client *client = data->client;
	int ret;

	/* 软件复位 */
	ret = ap3216c_write_reg(client, AP3216C_SYS_CONFIG,
				AP3216C_MODE_SW_RESET);
	if (ret) {
		dev_err(&client->dev, "软件复位失败: %d\n", ret);
		return ret;
	}

	/* 等待复位完成 */
	msleep(AP3216C_STARTUP_DELAY_MS);

	/* PS LED默认值：1个脉冲，100%最大驱动电流比例。 */
	ret = ap3216c_write_reg(client, AP3216C_PS_LED,
				AP3216C_PS_LED_DEFAULT);
	if (ret) {
		dev_err(&client->dev, "配置PS LED失败: %d\n", ret);
		return ret;
	}

	/* PS默认值：1T积分、x2增益、连续2次确认，输出10bit计数。 */
	ret = ap3216c_write_reg(client, AP3216C_PS_CFG,
				AP3216C_PS_CFG_DEFAULT);
	if (ret) {
		dev_err(&client->dev, "配置PS失败: %d\n", ret);
		return ret;
	}

	/* 配置 ALS: 16bit，默认量程 0~20661 lux（0.35 lux/LSB） */
	ret = ap3216c_write_reg(client, AP3216C_ALS_CFG, 0x00);
	if (ret) {
		dev_err(&client->dev, "配置ALS失败: %d\n", ret);
		return ret;
	}

	/* 启动ALS+PS模式 */
	ret = ap3216c_write_reg(client, AP3216C_SYS_CONFIG,
				AP3216C_MODE_ALS_PS);
	if (ret) {
		dev_err(&client->dev, "启动工作模式失败: %d\n", ret);
		return ret;
	}

	/* 等待第一次转换完成 */
	msleep(50);

	return 0;
}

/**
 * ap3216c_probe - I2C驱动探测函数
 * @client: I2C客户端
 *
 * 返回: 0成功，负数错误码
 */
static int ap3216c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct ap3216c_data *data;
	int ret;

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_BYTE_DATA))
		return dev_err_probe(dev, -EOPNOTSUPP,
				     "适配器不支持SMBus byte-data事务\n");

	/* 检查设备存在（读取SYS_CONFIG寄存器验证） */
	ret = i2c_smbus_read_byte_data(client, AP3216C_SYS_CONFIG);
	if (ret < 0) {
		return dev_err_probe(dev, ret, "无法读取AP3216C设备\n");
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
	ret = ap3216c_init_chip(data);
	if (ret) {
		dev_err(dev, "芯片初始化失败: %d\n", ret);
		return ret;
	}

	/* 配置IIO设备 */
	indio_dev->dev.parent = dev;
	indio_dev->name = "ap3216c";
	indio_dev->info = &ap3216c_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ap3216c_channels;
	indio_dev->num_channels = ARRAY_SIZE(ap3216c_channels);

	/* 可选：配置中断 */
	if (client->irq > 0) {
		data->irq = client->irq;
		/* 选择软件W1C，线程化handler才能显式清除已上报的状态位。 */
		ret = ap3216c_write_reg(client, AP3216C_INT_CLEAR_MANNER,
					AP3216C_INT_CLEAR_SW);
		if (!ret)
			ret = devm_request_threaded_irq(dev, data->irq,
							NULL, ap3216c_irq_handler,
							IRQF_TRIGGER_FALLING |
							IRQF_ONESHOT,
							AP3216C_DRV_NAME,
							indio_dev);
		if (ret) {
			dev_warn(dev, "中断模式不可用: %d\n", ret);
			if (ap3216c_write_reg(client,
					      AP3216C_INT_CLEAR_MANNER,
					      AP3216C_INT_CLEAR_AUTO))
				dev_warn(dev, "恢复自动中断清除模式失败\n");
			/* 中断可选，失败不退出 */
			data->irq = 0;
		} else {
			dev_info(dev, "中断模式已启用, irq=%d\n", data->irq);
		}
	}

	/* 注册IIO设备 */
	ret = devm_iio_device_register(dev, indio_dev);
	if (ret) {
		dev_err(dev, "IIO设备注册失败: %d\n", ret);
		(void)ap3216c_write_reg(client, AP3216C_SYS_CONFIG,
					AP3216C_MODE_POWERDOWN);
		return ret;
	}

	dev_info(dev, "AP3216C环境光/接近传感器驱动加载成功\n");

	return 0;
}

/**
 * ap3216c_remove - I2C驱动移除函数
 * @client: I2C客户端
 *
 * 设置芯片为关断模式以降低功耗。
 *
 */
static void ap3216c_remove(struct i2c_client *client)
{
	/* 设置关断模式 */
	i2c_smbus_write_byte_data(client, AP3216C_SYS_CONFIG,
				  AP3216C_MODE_POWERDOWN);

	dev_info(&client->dev, "AP3216C驱动卸载完成\n");
}

#ifdef CONFIG_PM_SLEEP
/**
 * ap3216c_suspend - 休眠回调
 * @dev: 设备指针
 *
 * 返回: 0成功
 */
static int ap3216c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	return i2c_smbus_write_byte_data(client, AP3216C_SYS_CONFIG,
					 AP3216C_MODE_POWERDOWN);
}

/**
 * ap3216c_resume - 唤醒回调
 * @dev: 设备指针
 *
 * 返回: 0成功
 */
static int ap3216c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);

	return i2c_smbus_write_byte_data(client, AP3216C_SYS_CONFIG,
					 AP3216C_MODE_ALS_PS);
}
#endif

static SIMPLE_DEV_PM_OPS(ap3216c_pm_ops, ap3216c_suspend, ap3216c_resume);

/* 设备树匹配表 */
static const struct of_device_id ap3216c_of_match[] = {
	{ .compatible = AP3216C_DRV_NAME, },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(of, ap3216c_of_match);

/* I2C设备ID表 */
static const struct i2c_device_id ap3216c_id[] = {
	{ "ap3216c", 0 },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(i2c, ap3216c_id);

/* I2C驱动结构体 */
static struct i2c_driver ap3216c_driver = {
	.driver = {
		.name		= "ap3216c-ocr",
		.of_match_table	= ap3216c_of_match,
		.pm		= &ap3216c_pm_ops,
	},
	.probe_new	= ap3216c_probe,
	.remove		= ap3216c_remove,
	.id_table	= ap3216c_id,
};

module_i2c_driver(ap3216c_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("AP3216C环境光/接近传感器IIO驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
