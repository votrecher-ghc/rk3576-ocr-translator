// SPDX-License-Identifier: GPL-2.0
/*
 * GPIO capture key driver for the RK3576 OCR translator.
 *
 * The driver converts one active-low GPIO button into Linux input events.
 * Mechanical switch debounce is handled in delayed-work context so GPIO
 * controllers whose get operation may sleep are supported safely.
 */

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#define OCR_CAPTURE_KEY_DRV_NAME             "ocr-gpio-capture-key"
#define OCR_CAPTURE_KEY_DEFAULT_DEBOUNCE_MS  20

struct ocr_capture_key {
	struct device *dev;
	struct gpio_desc *gpiod;
	struct input_dev *input;
	struct delayed_work debounce_work;
	const char *label;
	unsigned int code;
	unsigned int debounce_ms;
	int irq;
	bool last_pressed;
	bool irq_wake_enabled;
};

static void ocr_capture_key_report_work(struct work_struct *work)
{
	struct ocr_capture_key *key =
		container_of(to_delayed_work(work), struct ocr_capture_key,
			   debounce_work);
	int value;
	bool pressed;

	/*
	 * gpiod_get_value_cansleep() returns a logical value, so an
	 * active-low GPIO described by firmware is already inverted here.
	 */
	value = gpiod_get_value_cansleep(key->gpiod);
	if (value < 0) {
		dev_err_ratelimited(key->dev,
				    "failed to read capture key GPIO: %d\n",
				    value);
		return;
	}

	pressed = value != 0;
	if (pressed == key->last_pressed)
		return;

	key->last_pressed = pressed;
	input_report_key(key->input, key->code, pressed);
	input_sync(key->input);

	dev_dbg(key->dev, "%s: %s\n", key->label,
		pressed ? "pressed" : "released");
}

static irqreturn_t ocr_capture_key_irq(int irq, void *private)
{
	struct ocr_capture_key *key = private;

	/* Restart the debounce window on every electrical edge. */
	mod_delayed_work(system_wq, &key->debounce_work,
			 msecs_to_jiffies(key->debounce_ms));

	return IRQ_HANDLED;
}

static void ocr_capture_key_cleanup(void *private)
{
	struct ocr_capture_key *key = private;

	cancel_delayed_work_sync(&key->debounce_work);

	if (key->irq_wake_enabled) {
		disable_irq_wake(key->irq);
		key->irq_wake_enabled = false;
	}

	if (device_may_wakeup(key->dev))
		device_init_wakeup(key->dev, false);
}

static int ocr_capture_key_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ocr_capture_key *key;
	struct input_dev *input;
	u32 value;
	int ret;

	key = devm_kzalloc(dev, sizeof(*key), GFP_KERNEL);
	if (!key)
		return -ENOMEM;

	key->dev = dev;
	key->label = "OCR capture key";
	key->code = KEY_CAMERA;
	key->debounce_ms = OCR_CAPTURE_KEY_DEFAULT_DEBOUNCE_MS;

	device_property_read_string(dev, "label", &key->label);

	ret = device_property_read_u32(dev, "linux,code", &value);
	if (!ret)
		key->code = value;
	else if (ret != -EINVAL && ret != -ENODATA)
		return dev_err_probe(dev, ret, "failed to read linux,code\n");

	if (key->code == KEY_RESERVED || key->code > KEY_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "invalid input key code %u\n", key->code);

	ret = device_property_read_u32(dev, "debounce-interval-ms", &value);
	if (!ret)
		key->debounce_ms = value;
	else if (ret != -EINVAL && ret != -ENODATA)
		return dev_err_probe(dev, ret,
				     "failed to read debounce interval\n");

	if (!key->debounce_ms)
		return dev_err_probe(dev, -EINVAL,
				     "debounce interval must be non-zero\n");

	key->gpiod = devm_gpiod_get(dev, "key", GPIOD_IN);
	if (IS_ERR(key->gpiod))
		return dev_err_probe(dev, PTR_ERR(key->gpiod),
				     "failed to acquire key GPIO\n");

	key->irq = gpiod_to_irq(key->gpiod);
	if (key->irq < 0)
		return dev_err_probe(dev, key->irq,
				     "failed to map key GPIO to IRQ\n");

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	key->input = input;
	input->name = key->label;
	input->phys = "ocr-capture-key/input0";
	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	input_set_capability(input, EV_KEY, key->code);

	ret = devm_input_register_device(dev, input);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register input device\n");

	INIT_DELAYED_WORK(&key->debounce_work, ocr_capture_key_report_work);

	ret = devm_add_action_or_reset(dev, ocr_capture_key_cleanup, key);
	if (ret)
		return ret;

	ret = gpiod_get_value_cansleep(key->gpiod);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to read initial key state\n");
	key->last_pressed = ret != 0;

	ret = devm_request_irq(dev, key->irq, ocr_capture_key_irq,
			       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			       dev_name(dev), key);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request key IRQ\n");

	ret = device_init_wakeup(dev,
				 device_property_read_bool(dev, "wakeup-source"));
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to configure wakeup capability\n");

	platform_set_drvdata(pdev, key);

	dev_info(dev,
		 "registered %s on irq %d, code=%u, debounce=%u ms%s\n",
		 key->label, key->irq, key->code, key->debounce_ms,
		 device_may_wakeup(dev) ? ", wakeup enabled" : "");

	return 0;
}

static int ocr_capture_key_remove(struct platform_device *pdev)
{
	return 0;
}

static int __maybe_unused ocr_capture_key_suspend(struct device *dev)
{
	struct ocr_capture_key *key = dev_get_drvdata(dev);
	int ret;

	cancel_delayed_work_sync(&key->debounce_work);

	if (!device_may_wakeup(dev))
		return 0;

	ret = enable_irq_wake(key->irq);
	if (ret)
		return ret;

	key->irq_wake_enabled = true;
	return 0;
}

static int __maybe_unused ocr_capture_key_resume(struct device *dev)
{
	struct ocr_capture_key *key = dev_get_drvdata(dev);
	int ret = 0;

	if (key->irq_wake_enabled) {
		ret = disable_irq_wake(key->irq);
		key->irq_wake_enabled = false;
	}

	/* Re-sample after resume so the input state cannot remain stale. */
	mod_delayed_work(system_wq, &key->debounce_work,
			 msecs_to_jiffies(key->debounce_ms));

	return ret;
}

static SIMPLE_DEV_PM_OPS(ocr_capture_key_pm_ops,
			 ocr_capture_key_suspend, ocr_capture_key_resume);

static const struct of_device_id ocr_capture_key_of_match[] = {
	{ .compatible = "ocr,gpio-capture-key" },
	{ }
};
MODULE_DEVICE_TABLE(of, ocr_capture_key_of_match);

static struct platform_driver ocr_capture_key_driver = {
	.probe = ocr_capture_key_probe,
	.remove = ocr_capture_key_remove,
	.driver = {
		.name = OCR_CAPTURE_KEY_DRV_NAME,
		.of_match_table = ocr_capture_key_of_match,
		.pm = &ocr_capture_key_pm_ops,
	},
};
module_platform_driver(ocr_capture_key_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("GPIO capture key input driver for the RK3576 OCR translator");
MODULE_LICENSE("GPL");
