# PWM风扇驱动开发详解

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v1.1 | 2026-07-13 | 项目组 | 对齐用户态独占 PWM、自动发现与安全恢复策略 |
| v2.0 | 2026-07-16 | 项目组 | 补充交接日期和板端所有权验收要求 |

---

## 目录

- [1. 硬件连接](#1-硬件连接)
- [2. DTS 配置](#2-dts-配置)
- [3. 驱动实现](#3-驱动实现)
- [4. 散热策略](#4-散热策略)
- [5. 用户态控制](#5-用户态控制)

---

## 1. 硬件连接

- 当前 overlay 示例使用 RK3576 PWM11；最终通道必须按原理图与 base DTS 核对
- PWM 频率：25kHz（period=40000ns）
- 支持转速反馈（TACH，可选）

## 2. DTS 配置

```dts
&pwm11 {
    status = "okay";
    ocr,fan-pwm;
};
```

`ocr,fan-pwm` 是无值发现标记，不是 consumer compatible。当前默认方案由
`ocr_translator` 通过 sysfs 独占控制 PWM，所以同一通道不能同时创建内核
`pwm-fan`/`ocr,pwm-fan` consumer。

## 3. 驱动实现

默认控制器位于 `app/sensors/fan_ctrl.c`。它在任何 PWM sysfs 枚举或写入前获取
`/run/ocr-translator-fan.lock` 的非阻塞独占锁，按 DT 标记发现动态编号的
`pwmchip`，并在退出时恢复外部状态或 disable/unexport 自己拥有的通道。auto 模式
还会接管并清理上次崩溃遗留的专用 `pwm0` 导出。

仓库中的 `bsp/kernel/drivers/pwm_fan_ocr.c` 仅供改用内核 consumer 架构时选择；
若启用它，必须同时停用应用的 sysfs 控制，二者不能共存。

## 4. 散热策略

应用按 45/55/65/75°C 四个升档阈值调速，并使用 3°C 回差降档。温度设备缺失或
读取失败时切到满速，避免传感器故障导致静默停转。板端必须验证 PWM 极性、风扇
启转占空比、温度采样周期和掉电/异常退出行为。

## 5. 用户态控制

```bash
# 先停止应用，再按 DT 标记定位动态编号的 pwmchip
for chip in /sys/class/pwm/pwmchip*; do
    [ -e "$chip/device/of_node/ocr,fan-pwm" ] && printf '%s\n' "$chip"
done
```

不要在应用运行时手工 export 或写占空比；板端调试应通过应用日志确认实际 chip、
所有权模式和清理结果。

> 详见 [06-硬件接口参考/外设接线/PWM风扇接口.md](../06-硬件接口参考/外设接线/PWM风扇接口.md)

---

> 相关文档：[02-设备树与Overlay编写指南.md](02-设备树与Overlay编写指南.md)
