# 设备树与 Overlay 编写指南

| 版本 | 日期 | 作者 | 变更说明 |
|---|---|---|---|
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-07-16 | 项目组 | 对齐当前 overlay、互斥启用模式与板级核对边界 |

---

## 1. 当前原则

仓库中的 overlay 是**项目侧适配候选**，不是已经在鲁班猫3实板确认的最终 DTS。
所有 I2C/SPI/GPIO/PWM/CSI controller、pinctrl、IRQ 极性和 regulator 名称，都必须
与目标 SDK 的 base DTS、原理图和 40Pin 引脚表逐项核对。

默认构建和部署只使用 vendor base DTB。只有完成核对后，才显式选择一种 overlay
启用方式：

- `DT_OVERLAY_MODE=merged`
- `DT_OVERLAY_MODE=vendor-resource`

两种方式互斥，不能重复加载同一个设备节点。

## 2. 项目 overlay 清单

目录：

```text
bsp/kernel/arch/arm64/boot/dts/rockchip/overlays/
├── imx415-csi2.dtso
├── ap3216c.dtso
├── icm42688-spi.dtso
├── adt7410.dtso
├── pwm-fan.dtso
└── gpio-keys.dtso
```

| 文件 | 驱动匹配/用途 | 当前候选资源 | 状态 |
|---|---|---|---|
| `imx415-csi2.dtso` | `sony,imx415` | i2c4、CSI2 DPHY0、4 lane | 默认不构建；必须以出厂 media graph 为基线核对 |
| `ap3216c.dtso` | `ocr,ap3216c` | i2c4@0x1e、GPIO0_PA1 falling | 待原理图/base DTS 核对 |
| `icm42688-spi.dtso` | `ocr,icm42688` | spi0 CS0、24MHz、CPOL+CPHA、GPIO0_PB2 falling | 待 SPI mode/IRQ/pinctrl 核对 |
| `adt7410.dtso` | `ocr,adt7410` | i2c4@0x48 | 待总线和 regulator 核对 |
| `pwm-fan.dtso` | 用户态发现 marker `ocr,fan-pwm` | pwm11 | 待 PWM 通道、pinctrl、极性核对 |
| `gpio-keys.dtso` | `ocr,gpio-keys` | GPIO0_PC0、active-low、`KEY_CAMERA` | 待物理引脚和上下拉核对 |

## 3. Overlay 基本结构

项目采用 `/plugin/` + fragment 写法，例如：

```dts
/dts-v1/;
/plugin/;

/ {
    compatible = "rockchip,rk3576";

    fragment@0 {
        target = <&i2c4>;

        __overlay__ {
            status = "okay";

            ap3216c@1e {
                compatible = "ocr,ap3216c";
                reg = <0x1e>;
                status = "okay";
            };
        };
    };
};
```

`target` 引用的 label 必须确实存在于 vendor base DTS，并且 base DTB 构建时保留
overlay 所需的符号信息。

## 4. 当前外设片段

以下片段只用于解释仓库当前契约；最终值以核对后的 overlay 文件为准。

### 4.1 AP3216C

```dts
fragment@0 {
    target = <&i2c4>;

    __overlay__ {
        status = "okay";

        ap3216c: ap3216c@1e {
            compatible = "ocr,ap3216c";
            reg = <0x1e>;
            interrupt-parent = <&gpio0>;
            interrupts = <1 IRQ_TYPE_EDGE_FALLING>;
            vdd-supply = <&vcc_3v3_s3>;
            vled-supply = <&vcc_3v3_s3>;
            status = "okay";
        };
    };
};
```

### 4.2 ICM42688

```dts
fragment@0 {
    target = <&spi0>;

    __overlay__ {
        status = "okay";

        icm42688: icm42688@0 {
            compatible = "ocr,icm42688";
            reg = <0>;
            spi-max-frequency = <24000000>;
            spi-cpha;
            spi-cpol;
            interrupt-parent = <&gpio0>;
            interrupts = <10 IRQ_TYPE_EDGE_FALLING>;
            vdd-supply = <&vcc_3v3_s3>;
            vddio-supply = <&vcc_1v8_s3>;
            status = "okay";
        };
    };
};
```

### 4.3 ADT7410

```dts
fragment@0 {
    target = <&i2c4>;

    __overlay__ {
        status = "okay";

        adt7410: adt7410@48 {
            compatible = "ocr,adt7410";
            reg = <0x48>;
            #io-channel-cells = <1>;
            vdd-supply = <&vcc_3v3_s3>;
            status = "okay";
        };
    };
};
```

### 4.4 用户态 PWM 风扇

```dts
fragment@0 {
    target = <&pwm11>;

    __overlay__ {
        ocr,fan-pwm;
        status = "okay";
    };
};
```

`ocr,fan-pwm` 是无值发现 marker，不是 consumer compatible。当前应用通过 PWM
sysfs 独占控制该通道，因此不能再在同一 PWM 上创建 `pwm-fan` 或
`ocr,pwm-fan` consumer。

### 4.5 OCR 拍照按键

```dts
fragment@0 {
    target-path = "/";

    __overlay__ {
        ocr_gpio_keys: ocr-gpio-keys {
            compatible = "ocr,gpio-keys";
            label = "ocr-camera-key";
            gpios = <&gpio0 16 GPIO_ACTIVE_LOW>;
            linux,code = <KEY_CAMERA>;
            debounce-interval = <20>;
            status = "okay";
        };
    };
};
```

用户态按 `EV_KEY + KEY_CAMERA` capability 查找事件设备，不依赖 `event0`。

## 5. IMX415 特殊边界

IMX415 不只是一个 I2C sensor 节点，还依赖：

- sensor clock；
- regulator；
- reset/enable GPIO；
- CSI2 DPHY；
- CSI host；
- RKCIF/RKISP media graph；
- endpoint 和 lane/link-frequency；
- vendor camera/ISP 配置。

项目中的 `imx415-csi2.dtso` 未经目标 SDK 和实板验证，默认由
`scripts/build/make_image.sh` 跳过。只有确认 vendor 出厂 DTS 无法直接满足需求，
并完整核对 media graph 后，才设置：

```bash
export BUILD_UNVERIFIED_CAMERA_OVERLAY=1
```

## 6. 构建与启用

推荐使用项目脚本，不手工复制未验证的 DTBO：

```bash
export KERNEL_SRC=/path/to/vendor-sdk/kernel-6.1
bash scripts/build/build_kernel.sh

export OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-adt7410.dtbo ocr-pwm-fan.dtbo"
export DT_OVERLAY_MODE=merged
bash scripts/build/make_image.sh
```

使用 vendor resource 路径时：

```bash
export DT_OVERLAY_MODE=vendor-resource
export RK_PACK_SCRIPT=/path/to/vendor/pack-script
bash scripts/build/make_image.sh
```

## 7. 上板验收

至少检查：

```bash
dmesg | grep -Ei 'ap3216c|adt7410|icm42688|gpio|pwm|deferred|conflict'
find /sys/bus/iio/devices -maxdepth 2 -name name -exec sh -c 'printf "%s: " "$1"; cat "$1"' sh {} \;
cat /proc/bus/input/devices
media-ctl -p
modetest -c -p
```

通过条件包括：

- 没有重复节点、资源冲突或无法解释的 deferred probe；
- IIO `name` 唯一；
- `KEY_CAMERA` 唯一；
- PWM 只有一个 owner；
- overlay 启用前后 media graph 和显示拓扑符合预期。

---

> 相关文档：
>
> - [按键驱动开发详解](03-按键驱动开发详解.md)
> - [SDK 到板卡完整构建指南](../05-编译构建/09-SDK到板卡完整构建指南.md)
> - [当前实现状态与板端验收清单](../07-测试/08-当前实现状态与板端验收清单.md)
