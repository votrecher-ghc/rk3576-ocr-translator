# 设备树与Overlay编写指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. DTS 语法基础](#1-dts-语法基础)
- [2. RK3576 DTS 组织](#2-rk3576-dts-组织)
- [3. Overlay 机制](#3-overlay-机制)
- [4. 各外设 DTS 片段](#4-各外设-dts-片段)

---

## 1. DTS 语法基础

### 1.1 基本结构

```dts
/dts-v1/;
/ {
    node_name@address {
        compatible = "vendor,device";
        reg = <0x1e>;
        status = "okay";
        property = <value>;
    };
};
```

### 1.2 常用属性

| 属性        | 说明                       |
| ----------- | -------------------------- |
| compatible  | 兼容性字符串，匹配驱动     |
| reg         | 寄存器地址/I2C 地址        |
| interrupts  | 中断号                     |
| clocks      | 时钟引用                   |
| pinctrl-0   | 引脚复用配置               |
| status      | "okay"/"disabled"          |

### 1.3 引用与覆盖

```dts
&i2c2 {
    status = "okay";
    // 添加子节点
    new_device@1e { ... };
};
```

---

## 2. RK3576 DTS 组织

```
arch/arm64/boot/dts/rockchip/
├── rk3576.dtsi              // SoC 基础（CPU/中断/时钟/总线）
├── rk3576-pinctrl.dtsi      // 引脚复用
├── rk3576-lbc3.dts          // 鲁班猫3 板级
└── overlays/
    ├── Makefile
    ├── imx415.dtso
    ├── ap3216c.dtso
    ├── icm42688.dtso
    ├── adt7410.dtso
    ├── pwm-fan.dtso
    └── gpio-keys.dtso
```

---

## 3. Overlay 机制

### 3.1 Overlay 文件格式

```dts
/dts-v1/;
/plugin/;

&i2c2 {
    ap3216c@1e {
        compatible = "lbc,ap3216c";
        reg = <0x1e>;
        status = "okay";
    };
};
```

### 3.2 编译与加载

```bash
# 编译
dtc -@ -O dtb -o ap3216c.dtbo ap3216c.dtso

# 加载（运行时）
mkdir -p /sys/kernel/config/device-tree/overlays/ap3216c
cat ap3216c.dtbo > /sys/kernel/config/device-tree/overlays/ap3216c/dtbo
```

---

## 4. 各外设 DTS 片段

### 4.1 IMX415（CSI）

```dts
&csi2_dphy0 {
    status = "okay";
    ports {
        port@0 {
            ep: endpoint {
                remote-endpoint = <&imx415_out>;
                data-lanes = <1 2 3 4>;
            };
        };
    };
};

&i2c4 {
    imx415: imx415@1a {
        compatible = "sony,imx415";
        reg = <0x1a>;
        clocks = <&cru CLK_MIPI_CAMMOUT>;
        reset-gpios = <&gpio3 RK_PA5 GPIO_ACTIVE_LOW>;
        status = "okay";
    };
};
```

### 4.2 AP3216C（I2C）

```dts
&i2c2 {
    ap3216c@1e {
        compatible = "lbc,ap3216c";
        reg = <0x1e>;
        interrupt-parent = <&gpio3>;
        interrupts = <RK_PA1 IRQ_TYPE_EDGE_FALLING>;
        status = "okay";
    };
};
```

### 4.3 ICM42688（SPI）

```dts
&spi0 {
    status = "okay";
    icm42688@0 {
        compatible = "inv,icm42688";
        reg = <0>;
        spi-max-frequency = <10000000>;
        spi-cpha;
        interrupt-parent = <&gpio4>;
        interrupts = <RK_PA0 IRQ_TYPE_EDGE_RISING>;
        status = "okay";
    };
};
```

### 4.4 ADT7410（I2C）

```dts
&i2c2 {
    adt7410@48 {
        compatible = "adi,adt7410";
        reg = <0x48>;
        status = "okay";
    };
};
```

### 4.5 PWM 风扇

```dts
&pwm9 {
    status = "okay";
    fan: pwm-fan {
        compatible = "pwm-fan";
        cooling-cells = <2>;
        pwms = <&pwm9 0 50000 0>;
        cooling-levels = <0 30 60 80 100>;
        #cooling-cells = <2>;
        status = "okay";
    };
};
```

### 4.6 按键（GPIO）

```dts
gpio_keys: gpio-keys {
    compatible = "gpio-keys";
    pinctrl-0 = <&key_pin>;

    button_capture {
        label = "Capture";
        gpios = <&gpio0 RK_PA6 GPIO_ACTIVE_LOW>;
        linux,code = <KEY_ENTER>;
        debounce-interval = <10>;
    };
};
```

> 详见各驱动开发详解文档。

---

> 相关文档：[01-内核子系统概览.md](01-内核子系统概览.md)
