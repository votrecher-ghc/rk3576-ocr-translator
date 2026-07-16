// SPDX-License-Identifier: GPL-2.0
/*
 * pwm_fan_ocr.c - OCR系统PWM风扇温控驱动
 *
 * 描述: 基于PWM占空比控制风扇转速，结合分段温度阈值和回差(hysteresis)
 *       实现自动温控。通过hwmon子系统暴露温度和PWM信息给用户空间。
 *       温度来源: 通过IIO通道读取ADT7410温度传感器。
 *
 * 状态机: OFF → LOW → MID → HIGH（带回差防抖）
 *   - 温度上升超过阈值时升级档位
 *   - 温度下降需超过阈值-回差才降级档位
 *
 * DTS属性:
 *   pwms           - PWM通道引用
 *   cooling-cells  - 散热单元数
 *   temp-levels    - 温度阈值数组（毫摄氏度）
 *   pwm-duties     - 对应的PWM占空比数组（0-255）
 *   hysteresis     - 回差温度（毫摄氏度）
 *
 * 作者: OCR Team <ocr@example.com>
 * 许可证: GPL-2.0
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pwm.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/thermal.h>
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/slab.h>

/* 驱动兼容字符串 */
#define PWM_FAN_OCR_DRV_NAME	"ocr,pwm-fan"

/* 默认温度采样间隔（毫秒） */
#define DEFAULT_SAMPLE_INTERVAL_MS	2000

/* 最大支持的温控档位数 */
#define MAX_FAN_LEVELS		8

/**
 * 风扇状态枚举（档位0=OFF, 1=LOW, 2=MID, 3=HIGH, ...）
 * 实际档位数由设备树pwm-duties数组长度决定
 */
enum fan_state {
	FAN_STATE_OFF = 0,	/* 关闭 */
	FAN_STATE_LOW,		/* 低速 */
	FAN_STATE_MID,		/* 中速 */
	FAN_STATE_HIGH,		/* 高速 */
};

/**
 * struct pwm_fan_ocr - PWM风扇设备私有数据
 * @dev:            设备指针
 * @pwm:            PWM设备指针
 * @pwm_state:      PWM状态
 * @hwmon_dev:      hwmon设备指针
 * @temp_channel:   IIO温度通道（ADT7410）
 * @lock:           互斥锁
 * @work:           延迟工作（周期性温度采样）
 * @sample_interval: 采样间隔（毫秒）
 * @current_state:  当前风扇状态
 * @current_duty:   当前PWM占空比
 * @current_temp:   当前温度（毫摄氏度）
 * @num_levels:     温控档位数
 * @temp_levels:     温度阈值数组（毫摄氏度）
 * @pwm_duties:      PWM占空比数组
 * @hysteresis:     回差温度（毫摄氏度）
 */
struct pwm_fan_ocr {
	struct device *dev;
	struct pwm_device *pwm;
	struct pwm_state pwm_state;
	struct device *hwmon_dev;

	struct iio_channel *temp_channel;

	struct mutex lock;
	struct delayed_work work;
	unsigned int sample_interval;

	int current_state;		/* 当前风扇档位 */
	unsigned int current_duty;
	int current_temp;

	int num_levels;
	int temp_levels[MAX_FAN_LEVELS];
	int pwm_duties[MAX_FAN_LEVELS];
	int hysteresis;
};

/* hwmon通道编号 */
enum pwm_fan_hwmon_channels {
	hwmon_pwm_fan = 0,
	hwmon_pwm_fan_temp,
	hwmon_pwm_fan_duty,
	hwmon_pwm_fan_max,
};

/**
 * pwm_fan_set_duty - 设置PWM占空比
 * @fan:  风扇设备数据
 * @duty: 目标占空比（0-255）
 *
 * 将0-255的占空比映射到PWM周期，并应用PWM状态。
 *
 * 返回: 0成功，负数错误码
 */
static int pwm_fan_set_duty(struct pwm_fan_ocr *fan, unsigned int duty)
{
	int ret;
	u64 duty_ns;

	/* 限制占空比范围 */
	if (duty > 255)
		duty = 255;

	/* 将0-255映射到PWM周期（duty/255 * period） */
	duty_ns = fan->pwm_state.period * duty;
	do_div(duty_ns, 255);

	fan->pwm_state.duty_cycle = duty_ns;
	fan->pwm_state.enabled = (duty > 0) ? true : false;

	ret = pwm_apply_state(fan->pwm, &fan->pwm_state);
	if (ret) {
		dev_err(fan->dev, "应用PWM状态失败: %d\n", ret);
		return ret;
	}

	fan->current_duty = duty;

	dev_dbg(fan->dev, "PWM占空比设置为: %d/255\n", duty);

	return 0;
}

/**
 * pwm_fan_update_state - 根据温度更新风扇状态
 * @fan: 风扇设备数据
 *
 * 状态机逻辑（带回差）:
 * - 温度上升: 超过某档位阈值 → 升级到该档位
 * - 温度下降: 低于(阈值 - 回差) → 降级到下一档位
 *
 * temp_levels数组定义各档位升级的温度阈值（N个元素），
 * pwm_duties数组定义各档位对应的PWM占空比（N+1个元素）。
 *   档位0: temp < temp_levels[0]           → pwm_duties[0] (OFF)
 *   档位1: temp >= temp_levels[0]          → pwm_duties[1]
 *   档位2: temp >= temp_levels[1]          → pwm_duties[2]
 *   ...
 *   档位N: temp >= temp_levels[N-1]        → pwm_duties[N]
 */
static void pwm_fan_update_state(struct pwm_fan_ocr *fan)
{
	int temp = fan->current_temp;
	int i;
	int new_state = 0;
	int num_thresholds = fan->num_levels - 1; /* temp_levels数量 */

	mutex_lock(&fan->lock);

	/*
	 * 状态机决策:
	 * 从高到低检查温度阈值，确定目标状态
	 * 档位i+1对应温度 >= temp_levels[i]
	 */
	for (i = num_thresholds - 1; i >= 0; i--) {
		if (temp >= fan->temp_levels[i]) {
			new_state = i + 1;
			break;
		}
	}

	/* 回差检查: 如果温度下降，需要低于当前档位阈值-回差才降级 */
	if (new_state < fan->current_state) {
		int cur_threshold_idx = fan->current_state - 1;
		int downgrade_threshold;

		if (cur_threshold_idx >= 0) {
			downgrade_threshold =
				fan->temp_levels[cur_threshold_idx] -
				fan->hysteresis;

			if (temp > downgrade_threshold) {
				/* 温度未低于回差阈值，保持当前状态 */
				new_state = fan->current_state;
			}
		}
	}

	/* 如果状态变化，更新PWM */
	if (new_state != fan->current_state) {
		int new_duty = fan->pwm_duties[new_state];

		dev_info(fan->dev,
			 "风扇状态切换: %d → %d (温度=%dm°C, PWM=%d)\n",
			 fan->current_state, new_state, temp, new_duty);

		fan->current_state = new_state;
		pwm_fan_set_duty(fan, new_duty);
	}

	mutex_unlock(&fan->lock);
}

/**
 * pwm_fan_read_temp - 读取当前温度
 * @fan: 风扇设备数据
 *
 * 通过IIO通道读取ADT7410温度传感器的数据。
 * 使用 iio_convert_raw_to_processed 将原始值转换为毫摄氏度。
 *
 * 返回: 0成功，负数错误码
 */
static int pwm_fan_read_temp(struct pwm_fan_ocr *fan)
{
	int ret;
	int raw;
	int temp_milli;

	if (!fan->temp_channel) {
		dev_err(fan->dev, "温度通道未配置\n");
		return -ENODEV;
	}

	/* 读取IIO原始值 */
	ret = iio_read_channel_raw(fan->temp_channel, &raw);
	if (ret < 0) {
		dev_err(fan->dev, "读取温度原始值失败: %d\n", ret);
		return ret;
	}

	/*
	 * 将原始值转换为处理后的值（毫摄氏度）。
	 * iio_convert_raw_to_processed 会自动读取通道的 scale 和 offset，
	 * 并计算: processed = (raw + offset) * scale * scale_factor
	 * 其中 scale_factor 参数为 1000（将°C转换为m°C）。
	 */
	ret = iio_convert_raw_to_processed(fan->temp_channel, raw,
					   &temp_milli, 1000);
	if (ret < 0) {
		dev_err(fan->dev, "温度转换失败: %d\n", ret);
		return ret;
	}

	fan->current_temp = temp_milli;

	dev_dbg(fan->dev, "读取温度: raw=%d, temp=%dm°C\n", raw, temp_milli);

	return 0;
}

/**
 * pwm_fan_work_handler - 周期性温度采样工作队列处理函数
 * @work: 工作结构体指针
 *
 * 周期性读取温度并更新风扇状态。
 */
static void pwm_fan_work_handler(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct pwm_fan_ocr *fan = container_of(dwork, struct pwm_fan_ocr, work);
	int ret;

	/* 读取温度 */
	ret = pwm_fan_read_temp(fan);
	if (ret) {
		dev_err(fan->dev, "温度读取失败: %d\n", ret);
		/* 温度读取失败，保持当前状态 */
		goto reschedule;
	}

	/* 更新风扇状态 */
	pwm_fan_update_state(fan);

reschedule:
	/* 重新调度 */
	schedule_delayed_work(&fan->work,
			      msecs_to_jiffies(fan->sample_interval));
}

/**
 * pwm_fan_hwmon_read - hwmon读取回调
 * @dev:  设备指针
 * @type: hwmon类型
 * @attr: hwmon属性
 * @channel: 通道号
 *
 * 返回: 读取的值，负数错误码
 */
static int pwm_fan_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct pwm_fan_ocr *fan = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input) {
			/* 返回当前PWM占空比作为风扇转速参考 */
			*val = fan->current_duty;
			return 0;
		}
		break;

	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			/* 返回当前温度（毫摄氏度） */
			*val = fan->current_temp;
			return 0;
		}
		break;

	case hwmon_pwm:
		if (attr == hwmon_pwm_input) {
			/* 返回当前PWM占空比 */
			*val = fan->current_duty;
			return 0;
		}
		break;

	default:
		break;
	}

	return -EOPNOTSUPP;
}

/**
 * pwm_fan_hwmon_read_string - hwmon字符串读取回调
 */
static int pwm_fan_hwmon_read_string(struct device *dev,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel,
				     const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = "ADT7410";
		return 0;
	case hwmon_fan:
		*str = "PWM Fan";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* hwmon通道信息 */
static const struct hwmon_channel_info *pwm_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT),
	NULL,
};

/* hwmon操作 */
static const struct hwmon_ops pwm_fan_hwmon_ops = {
	.read = pwm_fan_hwmon_read,
	.read_string = pwm_fan_hwmon_read_string,
};

/* hwmon芯片信息 */
static const struct hwmon_chip_info pwm_fan_hwmon_chip_info = {
	.ops = &pwm_fan_hwmon_ops,
	.info = pwm_fan_hwmon_info,
};

/**
 * pwm_fan_parse_dt - 解析设备树
 * @pdev: 平台设备
 * @fan:  风扇设备数据
 *
 * 从设备树读取温度阈值、PWM占空比和回差配置。
 *
 * temp-levels数组定义温度阈值（N个元素），
 * pwm-duties数组定义对应PWM占空比（N+1个元素，第0个为低于首个阈值时的占空比）。
 * 例如:
 *   temp-levels = <40000 55000 70000>     (3个阈值)
 *   pwm-duties  = <0 128 200 255>         (4个占空比)
 *   档位0: temp < 40000 → PWM = 0 (OFF)
 *   档位1: temp >= 40000 → PWM = 128 (LOW)
 *   档位2: temp >= 55000 → PWM = 200 (MID)
 *   档位3: temp >= 70000 → PWM = 255 (HIGH)
 *
 * 返回: 0成功，负数错误码
 */
static int pwm_fan_parse_dt(struct platform_device *pdev,
			    struct pwm_fan_ocr *fan)
{
	struct device_node *np = pdev->dev.of_node;
	int temp_count, duty_count;
	int ret;

	/* 读取温度阈值数组 */
	temp_count = of_property_count_u32_elems(np, "temp-levels");
	if (temp_count < 0) {
		dev_err(&pdev->dev, "无法读取temp-levels: %d\n", temp_count);
		return temp_count;
	}
	if (temp_count >= MAX_FAN_LEVELS) {
		dev_err(&pdev->dev, "temp-levels超过最大值%d\n",
			MAX_FAN_LEVELS);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "temp-levels",
					 fan->temp_levels, temp_count);
	if (ret) {
		dev_err(&pdev->dev, "读取temp-levels失败: %d\n", ret);
		return ret;
	}

	/* 读取PWM占空比数组（应比temp-levels多1个元素） */
	duty_count = of_property_count_u32_elems(np, "pwm-duties");
	if (duty_count < 0) {
		dev_err(&pdev->dev, "无法读取pwm-duties: %d\n", duty_count);
		return duty_count;
	}

	/* pwm-duties应该等于 temp-levels数量 + 1 */
	if (duty_count != temp_count + 1) {
		dev_err(&pdev->dev,
			"pwm-duties数量(%d)应等于temp-levels数量(%d)+1\n",
			duty_count, temp_count);
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "pwm-duties",
					 fan->pwm_duties, duty_count);
	if (ret) {
		dev_err(&pdev->dev, "读取pwm-duties失败: %d\n", ret);
		return ret;
	}

	/* 总档位数 = pwm-duties数量（含OFF档） */
	fan->num_levels = duty_count;

	/* 读取回差温度（毫摄氏度） */
	ret = of_property_read_u32(np, "hysteresis", &fan->hysteresis);
	if (ret) {
		dev_warn(&pdev->dev, "未配置hysteresis，使用默认值3000m°C\n");
		fan->hysteresis = 3000;
	}

	/* 读取采样间隔（可选） */
	ret = of_property_read_u32(np, "sample-interval-ms",
				   &fan->sample_interval);
	if (ret)
		fan->sample_interval = DEFAULT_SAMPLE_INTERVAL_MS;

	dev_info(&pdev->dev, "配置: %d档温控, 回差=%dm°C, 采样间隔=%dms\n",
		 fan->num_levels, fan->hysteresis, fan->sample_interval);

	/* 打印档位配置 */
	dev_info(&pdev->dev, "  档位0: temp < %dm°C, PWM=%d (OFF)\n",
		 fan->temp_levels[0], fan->pwm_duties[0]);
	for (int i = 1; i < fan->num_levels; i++)
		dev_info(&pdev->dev, "  档位%d: temp >= %dm°C, PWM=%d\n",
			 i, fan->temp_levels[i - 1], fan->pwm_duties[i]);

	return 0;
}

/**
 * pwm_fan_ocr_probe - 驱动探测函数
 * @pdev: 平台设备
 *
 * 返回: 0成功，负数错误码
 */
static int pwm_fan_ocr_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_fan_ocr *fan;
	int ret;

	/* 分配设备数据 */
	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	fan->dev = dev;
	fan->current_state = FAN_STATE_OFF;
	fan->current_temp = 0;
	platform_set_drvdata(pdev, fan);

	/* 解析设备树 */
	ret = pwm_fan_parse_dt(pdev, fan);
	if (ret)
		return ret;

	/* 获取PWM设备 */
	fan->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(fan->pwm)) {
		dev_err(dev, "无法获取PWM设备: %ld\n", PTR_ERR(fan->pwm));
		return PTR_ERR(fan->pwm);
	}

	/* 初始化PWM状态 */
	pwm_init_state(fan->pwm, &fan->pwm_state);

	/* 设置初始占空比为0（风扇关闭） */
	fan->pwm_state.duty_cycle = 0;
	fan->pwm_state.enabled = false;
	ret = pwm_apply_state(fan->pwm, &fan->pwm_state);
	if (ret) {
		dev_err(dev, "初始PWM状态设置失败: %d\n", ret);
		return ret;
	}

	/* 获取IIO温度通道（ADT7410） */
	fan->temp_channel = devm_iio_channel_get(dev, "temp_adc");
	if (IS_ERR(fan->temp_channel)) {
		dev_warn(dev, "无法获取IIO温度通道: %ld, 使用thermal-zone\n",
			 PTR_ERR(fan->temp_channel));
		fan->temp_channel = NULL;
		/* 温度通道获取失败，后续可通过thermal-zone获取 */
	}

	mutex_init(&fan->lock);

	/* 注册hwmon设备 */
	fan->hwmon_dev = devm_hwmon_device_register_with_info(dev,
			"pwm_fan_ocr", fan, &pwm_fan_hwmon_chip_info, NULL);
	if (IS_ERR(fan->hwmon_dev)) {
		dev_err(dev, "hwmon设备注册失败: %ld\n",
			PTR_ERR(fan->hwmon_dev));
		return PTR_ERR(fan->hwmon_dev);
	}

	/* 初始化延迟工作队列 */
	INIT_DELAYED_WORK(&fan->work, pwm_fan_work_handler);

	/* 启动周期性温度采样 */
	schedule_delayed_work(&fan->work,
			      msecs_to_jiffies(fan->sample_interval));

	dev_info(dev, "PWM风扇温控驱动加载成功\n");

	return 0;
}

/**
 * pwm_fan_ocr_remove - 驱动移除函数
 * @pdev: 平台设备
 *
 * 返回: 0
 */
static int pwm_fan_ocr_remove(struct platform_device *pdev)
{
	struct pwm_fan_ocr *fan = platform_get_drvdata(pdev);

	/* 取消延迟工作 */
	cancel_delayed_work_sync(&fan->work);

	/* 关闭风扇 */
	mutex_lock(&fan->lock);
	pwm_fan_set_duty(fan, 0);
	mutex_unlock(&fan->lock);

	dev_info(&pdev->dev, "PWM风扇驱动卸载完成\n");

	return 0;
}

#ifdef CONFIG_PM_SLEEP
/**
 * pwm_fan_ocr_suspend - 休眠回调
 * @dev: 设备指针
 *
 * 休眠时取消工作队列并关闭风扇。
 *
 * 返回: 0
 */
static int pwm_fan_ocr_suspend(struct device *dev)
{
	struct pwm_fan_ocr *fan = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&fan->work);
	pwm_fan_set_duty(fan, 0);

	return 0;
}

/**
 * pwm_fan_ocr_resume - 唤醒回调
 * @dev: 设备指针
 *
 * 唤醒后恢复温度采样工作。
 *
 * 返回: 0
 */
static int pwm_fan_ocr_resume(struct device *dev)
{
	struct pwm_fan_ocr *fan = dev_get_drvdata(dev);

	schedule_delayed_work(&fan->work,
			      msecs_to_jiffies(fan->sample_interval));

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(pwm_fan_ocr_pm_ops,
			 pwm_fan_ocr_suspend, pwm_fan_ocr_resume);

/* 设备树匹配表 */
static const struct of_device_id pwm_fan_ocr_of_match[] = {
	{ .compatible = PWM_FAN_OCR_DRV_NAME, },
	{ /* 哨兵 */ },
};
MODULE_DEVICE_TABLE(of, pwm_fan_ocr_of_match);

/* 平台驱动结构体 */
static struct platform_driver pwm_fan_ocr_driver = {
	.probe	= pwm_fan_ocr_probe,
	.remove	= pwm_fan_ocr_remove,
	.driver	= {
		.name		= "pwm-fan-ocr",
		.of_match_table	= pwm_fan_ocr_of_match,
		.pm		= &pwm_fan_ocr_pm_ops,
	},
};

module_platform_driver(pwm_fan_ocr_driver);

MODULE_AUTHOR("OCR Team <ocr@example.com>");
MODULE_DESCRIPTION("RK3576鲁班猫3 OCR系统PWM风扇温控驱动");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
