# IMX415驱动适配与CSI配置

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. IMX415 简介](#1-imx415-简介)
- [2. 硬件连接](#2-硬件连接)
- [3. CSI 配置](#3-csi-配置)
- [4. DTS 配置](#4-dts-配置)
- [5. 驱动适配](#5-驱动适配)
- [6. V4L2 验证](#6-v4l2-验证)

---

## 1. IMX415 简介

IMX415 是 Sony 的 1/2.8 英寸 STARVIS CMOS 传感器，有效像素 4K（3840×2160），支持 4-lane MIPI CSI-2 输出。

## 2. 硬件连接

| IMX415 | RK3576     |
| ------ | ---------- |
| MIPI CLK± | CSI2_CLK± |
| MIPI DATA0± | CSI2_D0± |
| MIPI DATA1± | CSI2_D1± |
| MIPI DATA2± | CSI2_D2± |
| MIPI DATA3± | CSI2_D3± |
| SCL    | I2C4_SCL   |
| SDA    | I2C4_SDA   |
| RST    | GPIO3_A5   |
| PWDN   | GPIO3_A4   |

## 3. CSI 配置

```
IMX415 → MIPI-CSI2 → DPHY → CISU → ISP → V4L2(/dev/video0)
```

- CSI-2 4-lane
- DPHY: 1.5Gbps/lane
- ISP: 处理 RAW → NV12

## 4. DTS 配置

```dts
&i2c4 {
    imx415: imx415@1a {
        compatible = "sony,imx415";
        reg = <0x1a>;
        clocks = <&cru CLK_MIPI_CAMMOUT>;
        clock-names = "xvclk";
        reset-gpios = <&gpio3 RK_PA5 GPIO_ACTIVE_LOW>;
        pwdn-gpios = <&gpio3 RK_PA4 GPIO_ACTIVE_HIGH>;
        rockchip,camera-module-index = <0>;
        rockchip,camera-module-facing = "back";
        status = "okay";
        port {
            imx415_out: endpoint {
                remote-endpoint = <&mipi_in>;
                data-lanes = <1 2 3 4>;
                link-frequencies = /bits/ 64 <594000000>;
            };
        };
    };
};

&csi2_dphy0 {
    status = "okay";
    ports {
        port@0 { mipi_in: endpoint { remote-endpoint = <&imx415_out>; }; };
        port@1 { endpoint { remote-endpoint = <&csi_host_in>; }; };
    };
};

&rkcif { status = "okay"; };
&rkisp { status = "okay"; };
```

## 5. 驱动适配

- 驱动文件：`drivers/media/i2c/imx415.c`
- 寄存器初始化序列：4K@30fps、1080p@30fps 两套配置
- 曝光控制：V4L2_CID_EXPOSURE
- 增益控制：V4L2_CID_ANALOGUE_GAIN

## 6. V4L2 验证

```bash
# 查看格式
v4l2-ctl --device=/dev/video0 --list-formats-ext

# 采集测试
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
    --stream-mmap --stream-count=30
```

> 详见 [06-硬件接口参考/外设接线/IMX415-CSI接口.md](../06-硬件接口参考/外设接线/IMX415-CSI接口.md)

---

> 相关文档：[02-设备树与Overlay编写指南.md](02-设备树与Overlay编写指南.md)
