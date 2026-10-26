// SPDX-License-Identifier: GPL-2.0
/**
 * @file pwm_fan_ocr.c
 * 内核温控风扇：通过 IIO 读取 ADT7410 毫摄氏度温度，按设备树档位表和回差控制 PWM。
 * 延迟工作负责周期调速，hwmon 提供观察和手动模式；传感器失败时使用最高配置档位。
 */
/*
 * ADT7410 controlled PWM fan driver for LubanCat 3.
 *
 * The driver consumes one IIO temperature channel and controls a PWM output
 * using configurable temperature thresholds, PWM levels and hysteresis.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/hwmon.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pwm.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define OCR_PWM_FAN_DRV_NAME              "ocr-pwm-fan"
#define OCR_PWM_FAN_MAX_LEVELS            8
#define OCR_PWM_FAN_DEFAULT_POLL_MS       2000
#define OCR_PWM_FAN_DEFAULT_HYST_MC       3000
#define OCR_PWM_FAN_MAX_DUTY              255

#define OCR_PWM_FAN_MODE_MANUAL           1
#define OCR_PWM_FAN_MODE_AUTO             2

struct ocr_pwm_fan {
	struct device *dev; /**< 借用平台父设备，用于日志、属性读取和 devm 资源管理。 */
	struct pwm_device *pwm; /**< devm 取得的 PWM 消费者对象，内核温控独占控制该输出。 */
	struct pwm_state pwm_state; /**< 本驱动维护并提交的 PWM 状态，period/duty_cycle 单位均为纳秒。 */
	struct iio_channel *temp_chan; /**< devm 取得的 IIO 温度消费者通道，processed 返回毫摄氏度。 */
	struct delayed_work work; /**< 周期温控延迟任务，自动模式下读温调速，挂起/退出前同步取消。 */
	struct mutex lock; /**< 串行化温控任务与 hwmon 手动读写，保护档位、模式及 PWM 状态。 */

	u32 thresholds[OCR_PWM_FAN_MAX_LEVELS - 1]; /**< 升档温度门槛数组，毫摄氏度，严格递增；有效数为 num_levels-1。 */
	u32 duties[OCR_PWM_FAN_MAX_LEVELS]; /**< 各档位的 PWM 比例值，0..255；前 num_levels 项有效，255 才表示满占空比。 */
	unsigned int num_levels; /**< 设备树有效转速档位数，等于温度阈值数加一。 */
	unsigned int poll_ms; /**< 周期温控任务间隔，毫秒，默认 2000，必须大于 0。 */
	int hysteresis_mc; /**< 降档温度回差，毫摄氏度，默认 3000；从当前档阈值减去后比较。 */

	int temperature_mc; /**< 最近成功读取的温度，毫摄氏度，供 hwmon 读取；传感器失败时不更新。 */
	unsigned int current_level; /**< 当前自动档位下标，0..num_levels-1；与手动 PWM 比例值区分。 */
	unsigned int current_duty; /**< 最近成功应用的 0..255 PWM 比例值，不是纳秒或百分数。 */
	unsigned int mode; /**< hwmon 控制模式，1 手动、2 自动；自动任务只在自动模式更新占空比。 */
	bool suspended; /**< 设备挂起/退出标志，置位后不继续自动轮询，用于停机资源交接。 */
};

/* 把 0..255 档位值换算为当前周期的纳秒占空比；成功后才更新缓存值。 */
static int ocr_pwm_fan_apply_duty_locked(struct ocr_pwm_fan *fan,
					 unsigned int duty)
{
	u64 duty_cycle;
	int ret;

	if (duty > OCR_PWM_FAN_MAX_DUTY)
		return -EINVAL;

	/* period/duty_cycle 都用纳秒，duty 则是 0..255 比例；以 64 位中间值
	 * 相乘再 do_div，避免纳秒周期乘档位后在较窄整数中溢出。 */
	duty_cycle = fan->pwm_state.period * duty;
	do_div(duty_cycle, OCR_PWM_FAN_MAX_DUTY);

	fan->pwm_state.duty_cycle = duty_cycle;
	fan->pwm_state.enabled = duty != 0;

	ret = pwm_apply_state(fan->pwm, &fan->pwm_state);
	if (ret)
		return ret;

	fan->current_duty = duty;
	return 0;
}

/* 升温按阈值升档，降温必须低于当前阈值减回差后才允许降档。 */
static unsigned int ocr_pwm_fan_target_level(struct ocr_pwm_fan *fan,
					      int temp_mc)
{
	unsigned int target = 0;
	unsigned int i;

	for (i = 0; i + 1 < fan->num_levels; i++) {
		if (temp_mc >= fan->thresholds[i])
			target = i + 1;
		else
			break;
	}

	/*
	 * When cooling down, keep the current level until the temperature falls
	 * below the current level threshold minus the configured hysteresis.
	 */
	if (target < fan->current_level && fan->current_level > 0) {
		u32 threshold = fan->thresholds[fan->current_level - 1];

		if (temp_mc > (int)threshold - fan->hysteresis_mc)
			target = fan->current_level;
	}

	return target;
}

/* 读取 IIO 处理后的温度并调速；读取失败进入最高配置档位。 */
static int ocr_pwm_fan_update_auto_locked(struct ocr_pwm_fan *fan)
{
	unsigned int target;
	int temp_mc;
	int ret;

	/* IIO processed 已应用 ADT7410 的 raw/scale，得到毫摄氏度，直接与设备树
	 * temp-levels-millicelsius 比较；这里不能再套一次传感器换算公式。 */
	ret = iio_read_channel_processed(fan->temp_chan, &temp_mc);
	if (ret < 0) {
		/* 温度未知时取最后一个配置档位；实际 duty 来自 duties[]，
		 * 是否为物理满占空比取决于设备树最高档是否配置成 255。 */
		dev_err_ratelimited(fan->dev,
				    "temperature read failed: %d; forcing full fan\n",
				    ret);
		fan->current_level = fan->num_levels - 1;
		return ocr_pwm_fan_apply_duty_locked(fan,
						     fan->duties[fan->current_level]);
	}

	fan->temperature_mc = temp_mc;
	target = ocr_pwm_fan_target_level(fan, temp_mc);
	if (target == fan->current_level)
		return 0;

	ret = ocr_pwm_fan_apply_duty_locked(fan, fan->duties[target]);
	if (ret)
		return ret;

	dev_info(fan->dev,
		 "fan level %u -> %u, temperature=%d mC, pwm=%u/255\n",
		 fan->current_level, target, temp_mc, fan->duties[target]);
	fan->current_level = target;

	return 0;
}

/* 在设备锁内执行自动调速，非挂起状态下重新安排下次轮询。 */
static void ocr_pwm_fan_work(struct work_struct *work)
{
	struct ocr_pwm_fan *fan =
		container_of(to_delayed_work(work), struct ocr_pwm_fan, work);

	mutex_lock(&fan->lock);
	if (!fan->suspended && fan->mode == OCR_PWM_FAN_MODE_AUTO)
		ocr_pwm_fan_update_auto_locked(fan);
	mutex_unlock(&fan->lock);

	if (!fan->suspended)
		schedule_delayed_work(&fan->work,
				      msecs_to_jiffies(fan->poll_ms));
}

static umode_t ocr_pwm_fan_is_visible(const void *data,
				      enum hwmon_sensor_types type,
				      u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input || attr == hwmon_pwm_enable)
			return 0644;
		break;
	default:
		break;
	}

	return 0;
}

static int ocr_pwm_fan_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel, long *val)
{
	struct ocr_pwm_fan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			*val = fan->temperature_mc;
			mutex_unlock(&fan->lock);
			return 0;
		}
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input) {
			*val = fan->current_duty;
			mutex_unlock(&fan->lock);
			return 0;
		}
		if (attr == hwmon_pwm_enable) {
			*val = fan->mode;
			mutex_unlock(&fan->lock);
			return 0;
		}
		break;
	default:
		break;
	}

	mutex_unlock(&fan->lock);
	return -EOPNOTSUPP;
}

static int ocr_pwm_fan_hwmon_write(struct device *dev,
				   enum hwmon_sensor_types type,
				   u32 attr, int channel, long val)
{
	struct ocr_pwm_fan *fan = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&fan->lock);

	if (type != hwmon_pwm) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > OCR_PWM_FAN_MAX_DUTY) {
			ret = -EINVAL;
			break;
		}
		fan->mode = OCR_PWM_FAN_MODE_MANUAL;
		ret = ocr_pwm_fan_apply_duty_locked(fan, val);
		break;

	case hwmon_pwm_enable:
		if (val != OCR_PWM_FAN_MODE_MANUAL &&
		    val != OCR_PWM_FAN_MODE_AUTO) {
			ret = -EINVAL;
			break;
		}

		fan->mode = val;
		if (fan->mode == OCR_PWM_FAN_MODE_AUTO)
			ret = ocr_pwm_fan_update_auto_locked(fan);
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

out:
	mutex_unlock(&fan->lock);
	return ret;
}

static const struct hwmon_ops ocr_pwm_fan_hwmon_ops = {
	.is_visible = ocr_pwm_fan_is_visible,
	.read = ocr_pwm_fan_hwmon_read,
	.write = ocr_pwm_fan_hwmon_write,
};

/* 6.1 的 hwmon_chip_info.info 是 const struct hwmon_channel_info **,
 * 内层 const 要到更高版本内核才引入, 这里不能加。
 */
static const struct hwmon_channel_info *ocr_pwm_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL,
};

static const struct hwmon_chip_info ocr_pwm_fan_chip_info = {
	.ops = &ocr_pwm_fan_hwmon_ops,
	.info = ocr_pwm_fan_hwmon_info,
};

/* 校验阈值、档位和轮询参数，拒绝非递增温度阈值或超出范围的占空比。 */
static int ocr_pwm_fan_parse_dt(struct platform_device *pdev,
				struct ocr_pwm_fan *fan)
{
	struct device *dev = &pdev->dev;
	int threshold_count;
	int duty_count;
	int ret;
	unsigned int i;

	threshold_count = device_property_count_u32(dev,
						   "ocr,temp-levels-millicelsius");
	if (threshold_count < 1 ||
	    threshold_count >= OCR_PWM_FAN_MAX_LEVELS)
		return dev_err_probe(dev, -EINVAL,
				     "invalid ocr,temp-levels-millicelsius\n");

	duty_count = device_property_count_u32(dev, "ocr,pwm-duty-levels");
	if (duty_count != threshold_count + 1)
		return dev_err_probe(dev, -EINVAL,
				     "pwm level count must equal threshold count + 1\n");

	ret = device_property_read_u32_array(dev,
					     "ocr,temp-levels-millicelsius",
					     fan->thresholds,
					     threshold_count);
	if (ret)
		return ret;

	ret = device_property_read_u32_array(dev, "ocr,pwm-duty-levels",
					     fan->duties, duty_count);
	if (ret)
		return ret;

	for (i = 1; i < threshold_count; i++) {
		if (fan->thresholds[i] <= fan->thresholds[i - 1])
			return dev_err_probe(dev, -EINVAL,
					     "temperature thresholds must increase\n");
	}

	for (i = 0; i < duty_count; i++) {
		if (fan->duties[i] > OCR_PWM_FAN_MAX_DUTY)
			return dev_err_probe(dev, -EINVAL,
					     "PWM duty must be in the range 0..255\n");
	}

	fan->num_levels = duty_count;
	fan->poll_ms = OCR_PWM_FAN_DEFAULT_POLL_MS;
	fan->hysteresis_mc = OCR_PWM_FAN_DEFAULT_HYST_MC;

	device_property_read_u32(dev, "ocr,poll-interval-ms", &fan->poll_ms);
	device_property_read_u32(dev, "ocr,hysteresis-millicelsius",
				 &fan->hysteresis_mc);

	if (!fan->poll_ms)
		return dev_err_probe(dev, -EINVAL,
				     "poll interval must be non-zero\n");

	return 0;
}

/* 同步取消延迟工作后设置退出状态，防止资源释放后仍有调速任务运行。 */
static void ocr_pwm_fan_cleanup(void *private)
{
	struct ocr_pwm_fan *fan = private;

	cancel_delayed_work_sync(&fan->work);

	mutex_lock(&fan->lock);
	fan->suspended = true;
	ocr_pwm_fan_apply_duty_locked(fan, 0);
	mutex_unlock(&fan->lock);
}

static int ocr_pwm_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ocr_pwm_fan *fan;
	struct device *hwmon;
	int ret;

	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	fan->dev = dev;
	fan->mode = OCR_PWM_FAN_MODE_AUTO;
	mutex_init(&fan->lock);
	platform_set_drvdata(pdev, fan);

	ret = ocr_pwm_fan_parse_dt(pdev, fan);
	if (ret)
		return ret;

	fan->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(fan->pwm))
		return dev_err_probe(dev, PTR_ERR(fan->pwm),
				     "failed to acquire PWM\n");

	pwm_init_state(fan->pwm, &fan->pwm_state);
	if (!fan->pwm_state.period)
		return dev_err_probe(dev, -EINVAL,
				     "PWM period is zero\n");

	fan->temp_chan = devm_iio_channel_get(dev, "temp");
	if (IS_ERR(fan->temp_chan))
		return dev_err_probe(dev, PTR_ERR(fan->temp_chan),
				     "failed to acquire ADT7410 temperature channel\n");

	INIT_DELAYED_WORK(&fan->work, ocr_pwm_fan_work);

	mutex_lock(&fan->lock);
	ret = ocr_pwm_fan_apply_duty_locked(fan, 0);
	mutex_unlock(&fan->lock);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialise PWM output\n");

	ret = devm_add_action_or_reset(dev, ocr_pwm_fan_cleanup, fan);
	if (ret)
		return ret;

	hwmon = devm_hwmon_device_register_with_info(dev, OCR_PWM_FAN_DRV_NAME,
						     fan,
						     &ocr_pwm_fan_chip_info,
						     NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	schedule_delayed_work(&fan->work, 0);

	dev_info(dev, "ADT7410 controlled PWM fan registered\n");
	return 0;
}

static int ocr_pwm_fan_remove(struct platform_device *pdev)
{
	return 0;
}

static int __maybe_unused ocr_pwm_fan_suspend(struct device *dev)
{
	struct ocr_pwm_fan *fan = dev_get_drvdata(dev);
	int ret;

	cancel_delayed_work_sync(&fan->work);

	mutex_lock(&fan->lock);
	fan->suspended = true;
	ret = ocr_pwm_fan_apply_duty_locked(fan, 0);
	mutex_unlock(&fan->lock);

	return ret;
}

static int __maybe_unused ocr_pwm_fan_resume(struct device *dev)
{
	struct ocr_pwm_fan *fan = dev_get_drvdata(dev);

	mutex_lock(&fan->lock);
	fan->suspended = false;
	mutex_unlock(&fan->lock);

	schedule_delayed_work(&fan->work, 0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(ocr_pwm_fan_pm_ops,
			 ocr_pwm_fan_suspend, ocr_pwm_fan_resume);

static const struct of_device_id ocr_pwm_fan_of_match[] = {
	{ .compatible = "ocr,pwm-fan-controller" },
	{ }
};
MODULE_DEVICE_TABLE(of, ocr_pwm_fan_of_match);

static struct platform_driver ocr_pwm_fan_driver = {
	.probe = ocr_pwm_fan_probe,
	.remove = ocr_pwm_fan_remove,
	.driver = {
		.name = OCR_PWM_FAN_DRV_NAME,
		.of_match_table = ocr_pwm_fan_of_match,
		.pm = &ocr_pwm_fan_pm_ops,
	},
};
module_platform_driver(ocr_pwm_fan_driver);

MODULE_AUTHOR("GHC");
MODULE_DESCRIPTION("ADT7410 controlled PWM fan driver");
MODULE_LICENSE("GPL");
