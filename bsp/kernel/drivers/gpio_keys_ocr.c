// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_keys_ocr.c - OCR系统GPIO按键驱动
 *
 * 描述: 基于GPIO中断 + 消抖定时器的按键驱动，通过Input子系统上报按键事件。
 *       针对RK3576鲁班猫3开发板的40Pin扩展接口设计，用于OCR系统的拍照触发按键。
 *
 * 作者: OCR Team <ocr@example.com>
 * 许可证: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/pm.h>
#include <uapi/linux/input-event-codes.h>

/* 驱动兼容字符串 */
#define GPIO_KEYS_OCR_DRV_NAME	"ocr,gpio-keys"

/* 默认消抖间隔（毫秒） */
#define DEFAULT_DEBOUNCE_INTERVAL_MS	20

/**
 * struct ocr_gpio_key - 单个按键的运行时数据
 * @input:              指向input设备的指针
 * @gpio:               GPIO编号
 * @irq:                中断号
 * @code:               上报的按键码（如 KEY_CAMERA）
 * @active_low:         是否低电平有效
 * @debounce_interval:  消抖间隔（毫秒）
 * @debounce_timer:     消抖定时器
 * @state:              当前按键状态（0=释放，1=按下）
 * @label:              按键标签字符串
 */
struct ocr_gpio_key {
	struct input_dev *input;
	int gpio;
	int irq;
	unsigned int code;
	bool active_low;
	unsigned int debounce_interval;
	struct timer_list debounce_timer;
	int state;
	const char *label;
};

/**
 * ocr_gpio_key_debounce_timer - 消抖定时器回调函数
 * @t: 定时器结构体指针
 *
 * 定时器到期后重新读取GPIO电平，确认按键状态并上报input事件。
 * 若电平与触发中断时一致，则上报按键按下/释放事件。
 */
static void ocr_gpio_key_debounce_timer(struct timer_list *t)
{
	struct ocr_gpio_key *key = from_timer(key, t, debounce_timer);
	int current_state;
	int report_state;

	/* 读取当前GPIO电平 */
	current_state = gpio_get_value_cansleep(key->gpio);

	/* 根据active_low转换逻辑电平 */
	report_state = key->active_low ? !current_state : current_state;

	if (report_state != key->state) {
		key->state = report_state;

		/* 上报按键事件 */
		input_event(key->input, EV_KEY, key->code, !!key->state);
		input_sync(key->input);

		dev_dbg(&key->input->dev,
			"按键[%s] %s (gpio=%d, code=0x%x)\n",
			key->label,
			key->state ? "按下" : "释放",
			key->gpio, key->code);
	}
}

/**
 * ocr_gpio_key_isr - GPIO中断处理函数
 * @irq:    触发中断的中断号
 * @dev_id: 传递的设备私有数据（struct ocr_gpio_key指针）
 *
 * 中断触发后不直接处理按键状态，而是启动消抖定时器，
 * 延迟一段时间后再读取GPIO电平，避免机械抖动。
 *
 * 返回: IRQ_HANDLED 表示中断已处理
 */
static irqreturn_t ocr_gpio_key_isr(int irq, void *dev_id)
{
	struct ocr_gpio_key *key = dev_id;

	/* 启动消抖定时器，将jiffies换算为毫秒 */
	mod_timer(&key->debounce_timer,
		  jiffies + msecs_to_jiffies(key->debounce_interval));

	return IRQ_HANDLED;
}

/**
 * ocr_gpio_keys_parse_dt - 解析设备树
 * @pdev: 平台设备指针
 * @key:  按键数据结构指针
 *
 * 从设备树节点中读取GPIO、按键码、消抖间隔、标签等属性。
 *
 * 返回: 0成功，负数错误码失败
 */
static int ocr_gpio_keys_parse_dt(struct platform_device *pdev,
				  struct ocr_gpio_key *key)
{
	struct device_node *np = pdev->dev.of_node;
	enum of_gpio_flags flags;
	int ret;

	if (!np) {
		dev_err(&pdev->dev, "缺少设备树节点\n");
		return -ENODEV;
	}

	/* 读取GPIO及标志 */
	key->gpio = of_get_gpio_flags(np, 0, &flags);
	if (key->gpio < 0) {
		dev_err(&pdev->dev, "无法获取GPIO: %d\n", key->gpio);
		return key->gpio;
	}
	key->active_low = (flags & OF_GPIO_ACTIVE_LOW) ? true : false;

	/* 读取按键码 */
	ret = of_property_read_u32(np, "linux,code", &key->code);
	if (ret) {
		dev_err(&pdev->dev, "无法读取linux,code属性: %d\n", ret);
		return ret;
	}

	/* 读取消抖间隔，可选属性，默认20ms */
	ret = of_property_read_u32(np, "debounce-interval",
				   &key->debounce_interval);
	if (ret)
		key->debounce_interval = DEFAULT_DEBOUNCE_INTERVAL_MS;

	/* 读取标签 */
	ret = of_property_read_string(np, "label", &key->label);
	if (ret)
		key->label = "ocr-gpio-key";

	return 0;
}

/**
 * ocr_gpio_keys_probe - 驱动探测函数
 * @pdev: 平台设备指针
 *
 * 执行流程:
 * 1. 解析设备树获取GPIO和按键配置
 * 2. 申请GPIO并设置方向
 * 3. 分配input设备
 * 4. 设置input设备能力
 * 5. 注册input设备
 * 6. 申请中断
 * 7. 初始化消抖定时器
 *
 * 返回: 0成功，负数错误码失败
 */
static int ocr_gpio_keys_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ocr_gpio_key *key;
	struct input_dev *input;
	int ret;

	/* 分配按键数据结构 */
	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	/* 解析设备树 */
	ret = ocr_gpio_keys_parse_dt(pdev, key);
	if (ret)
		return ret;

	/* 申请GPIO */
	ret = devm_gpio_request(dev, key->gpio, key->label);
	if (ret) {
		dev_err(dev, "无法申请GPIO %d: %d\n", key->gpio, ret);
		return ret;
	}

	/* 设置GPIO为输入 */
	ret = gpio_direction_input(key->gpio);
	if (ret) {
		dev_err(dev, "无法设置GPIO %d为输入: %d\n", key->gpio, ret);
		return ret;
	}

	/* 分配input设备 */
	input = devm_input_allocate_device(dev);
	if (!input) {
		dev_err(dev, "无法分配input设备\n");
		return -ENOMEM;
	}

	key->input = input;

	/* 配置input设备 */
	input->name = "OCR GPIO Keys";
	input->phys = "gpio-keys-ocr/input0";
	input->dev.parent = dev;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	/* 设置按键能力 */
	__set_bit(EV_KEY, input->evbit);
	__set_bit(key->code, input->keybit);

	/* 初始化消抖定时器 */
	timer_setup(&key->debounce_timer, ocr_gpio_key_debounce_timer, 0);

	/* 获取初始按键状态 */
	key->state = gpio_get_value_cansleep(key->gpio);
	key->state = key->active_low ? !key->state : key->state;

	/* 注册input设备 */
	ret = input_register_device(input);
	if (ret) {
		dev_err(dev, "无法注册input设备: %d\n", ret);
		return ret;
	}

	/* 获取中断号 */
	key->irq = gpio_to_irq(key->gpio);
	if (key->irq < 0) {
		dev_err(dev, "无法获取GPIO %d的中断号: %d\n",
			key->gpio, key->irq);
		return key->irq;
	}

	/* 申请中断，使用双边沿触发 */
	ret = devm_request_irq(dev, key->irq, ocr_gpio_key_isr,
			       IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
			       key->label, key);
	if (ret) {
		dev_err(dev, "无法申请中断 %d: %d\n", key->irq, ret);
		return ret;
	}

	/* 禁用中断作为唤醒源默认（可选） */
	/* enable_irq_wake(key->irq); */

	platform_set_drvdata(pdev, key);

	dev_info(dev, "OCR GPIO按键驱动加载成功: gpio=%d, irq=%d, code=0x%x, label=%s\n",
		 key->gpio, key->irq, key->code, key->label);

	return 0;
}

/**
 * ocr_gpio_keys_remove - 驱动移除函数
 * @pdev: 平台设备指针
 *
 * 清理资源：删除定时器、释放中断、注销input设备。
 * 使用devm_*管理的资源会自动释放。
 *
 * 返回: 0
 */
static int ocr_gpio_keys_remove(struct platform_device *pdev)
{
	struct ocr_gpio_key *key = platform_get_drvdata(pdev);

	/* 删除消抖定时器 */
	if (key)
		del_timer_sync(&key->debounce_timer);

	dev_info(&pdev->dev, "OCR GPIO按键驱动卸载完成\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
/**
 * ocr_gpio_keys_suspend - 系统休眠回调
 * @dev: 设备指针
 *
 * 返回: 0
 */
static int ocr_gpio_keys_suspend(struct device *dev)
{
	/* 可在此启用中断唤醒源 */
	return 0;
}

/**
 * ocr_gpio_keys_resume - 系统唤醒回调
 * @dev: 设备指针
 *
 * 返回: 0
 */
static int ocr_gpio_keys_resume(struct device *dev)
{
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(ocr_gpio_keys_pm_ops,
			 ocr_gpio_keys_suspend, ocr_gpio_keys_resume);

/* 设备树匹配表 */
static const struct of_device_id ocr_gpio_keys_of_match[] = {
	{ .compatible = GPIO_KEYS_OCR_DRV_NAME, },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(of, ocr_gpio_keys_of_match);

/* 平台驱动结构体 */
static struct platform_driver ocr_gpio_keys_driver = {
	.probe	= ocr_gpio_keys_probe,
	.remove	= ocr_gpio_keys_remove,
	.driver	= {
		.name		= "ocr-gpio-keys",
		.of_match_table	= ocr_gpio_keys_of_match,
		.pm		= &ocr_gpio_keys_pm_ops,
	},
};

module_platform_driver(ocr_gpio_keys_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("RK3576鲁班猫3 OCR系统GPIO按键驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
