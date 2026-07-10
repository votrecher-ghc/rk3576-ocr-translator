# PWM风扇驱动开发详解

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 硬件连接](#1-硬件连接)
- [2. DTS 配置](#2-dts-配置)
- [3. 驱动实现](#3-驱动实现)
- [4. 散热策略](#4-散热策略)
- [5. 用户态控制](#5-用户态控制)

---

## 1. 硬件连接

- PWM 风扇连接至 RK3576 PWM9 通道
- PWM 频率：20kHz（period=50000ns）
- 支持转速反馈（TACH，可选）

## 2. DTS 配置

```dts
&pwm9 {
    status = "okay";
    pinctrl-0 = <&pwm9m0_pins>;
    pinctrl-names = "default";
};

fan: pwm-fan {
    compatible = "pwm-fan";
    pwms = <&pwm9 0 50000 0>;
    cooling-levels = <0 30 60 80 100>;
    #cooling-cells = <2>;
    status = "okay";
};
```

## 3. 驱动实现

> 内核内置 `pwm-fan` 驱动（`drivers/hwmon/pwm-fan.c`），无需自写。

```c
// 如需自定义，关键 PWM API:
struct pwm_device *pwm = devm_pwm_get(dev, NULL);
pwm_config(pwm, duty_ns, period_ns);  // 设置占空比
pwm_enable(pwm);
```

## 4. 散热策略

结合 thermal-zone 与 cooling-device 实现自动调速：

```dts
thermal-zones {
    soc_thermal {
        polling-delay-passive = <1000>;
        trips {
            fan_trip0: trip-point0 {
                temperature = <45000>;
                hysteresis = <2000>;
                type = "active";
            };
            fan_trip1: trip-point1 {
                temperature = <55000>;
                hysteresis = <2000>;
                type = "active";
            };
        };
        cooling-maps {
            map0 { trip = <&fan_trip0>; cooling-device = <&fan 1 4>; };
            map1 { trip = <&fan_trip1>; cooling-device = <&fan 2 4>; };
        };
    };
};
```

## 5. 用户态控制

```bash
# 导出 PWM
echo 0 > /sys/class/pwm/pwmchip9/export
# 设置周期（ns）
echo 50000 > /sys/class/pwm/pwmchip9/pwm0/period
# 设置占空比（ns），30% = 15000
echo 15000 > /sys/class/pwm/pwmchip9/pwm0/duty_cycle
# 使能
echo 1 > /sys/class/pwm/pwmchip9/pwm0/enable
```

> 详见 [06-硬件接口参考/外设接线/PWM风扇接口.md](../06-硬件接口参考/外设接线/PWM风扇接口.md)

---

> 相关文档：[02-设备树与Overlay编写指南.md](02-设备树与Overlay编写指南.md)
