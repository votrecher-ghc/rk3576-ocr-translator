# sensors 模块 API

| 版本 | 日期 | 作者 | 变更说明 |
|---|---|---|---|
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v1.1 | 2026-07-10 | 项目组 | AP3216C 仅用于屏幕亮度调节 |
| v2.0 | 2026-07-16 | 项目组 | 对齐真实结构、IIO 动态发现、温度 ABI 和 PWM 所有权边界 |

---

## 1. 文件与职责

| 文件 | 职责 |
|---|---|
| `iio_discovery.h/.c` | 按 IIO `name` 唯一查找 sysfs 和 `/dev` 路径 |
| `light_sensor.h/.c` | AP3216C illuminance input 或 raw/scale 读取 |
| `brightness_ctrl.h/.c` | lux 分段映射、回差和平滑背光写入 |
| `temp_sensor.h/.c` | ADT7410 `in_temp_input` 或 raw/offset/scale 读取 |
| `fan_ctrl.h/.c` | 25kHz 四档 PWM、进程锁、外部状态恢复和故障安全 |

摄像头曝光由 IMX415/V4L2 AE 处理，不与 AP3216C 联动。

## 2. IIO 发现 API

```c
int ocr_iio_find_device(const char *iio_name,
                        char *sysfs_path,
                        size_t sysfs_size,
                        char *dev_path,
                        size_t dev_size);
```

按 `/sys/bus/iio/devices/iio:device*/name` 的完整内容匹配。只有唯一匹配才成功；
编号变化、零匹配或重复名称都会明确返回错误。

## 3. 环境光与背光 API

```c
int ocr_light_sensor_init(ocr_light_sensor_t *sensor,
                          const char *sysfs_path);
int ocr_light_sensor_read(ocr_light_sensor_t *sensor, int *lux);

int ocr_brightness_init(ocr_brightness_t *ctrl,
                        const char *sysfs_path);
int ocr_brightness_update(ocr_brightness_t *ctrl, int lux);
int ocr_brightness_set(ocr_brightness_t *ctrl, int level);
int ocr_brightness_get(ocr_brightness_t *ctrl);
```

光照读取顺序：

1. `in_illuminance_input`；
2. `in_intensity_both_input`；
3. `in_illuminance_raw * in_illuminance_scale`；
4. `in_intensity_both_raw * in_intensity_scale`。

raw 与 scale 必须成对存在，避免把 ADC count 当成 lux。

## 4. 温度 API

```c
int ocr_temp_sensor_init(ocr_temp_sensor_t *sensor,
                         const char *sysfs_path);
int ocr_temp_sensor_read(ocr_temp_sensor_t *sensor,
                         int *temp_mdeg);
```

读取顺序：

1. `in_temp_input`；
2. `temp1_input` 兼容路径；
3. `(in_temp_raw + in_temp_offset) * in_temp_scale`。

输出单位固定为毫摄氏度。ADT7410 项目驱动的 scale 是
`125/16 = 7.8125 m°C/LSB`。

## 5. PWM 风扇 API

```c
int ocr_fan_ctrl_init(ocr_fan_ctrl_t *ctrl,
                      const char *pwm_path);
int ocr_fan_ctrl_update(ocr_fan_ctrl_t *ctrl,
                        int temp_mdeg);
int ocr_fan_ctrl_set_level(ocr_fan_ctrl_t *ctrl,
                           int level);
int ocr_fan_ctrl_destroy(ocr_fan_ctrl_t *ctrl);
```

`pwm_path` 可以是：

- `"auto"`：按 DT `ocr,fan-pwm` marker 唯一发现；
- `/sys/class/pwm/pwmchipX/pwmY`；
- 对应的 `duty_cycle` 路径。

主配置的 `pwm_fan_path` 还可设为 `"kernel"`（默认），此时主程序不调用以上
用户态 PWM API，而由 `ocr-pwm-fan` 模块独占 PWM、消费 ADT7410 IIO 温度并完成
闭环温控。`kernel` 与 `auto`/显式 sysfs 路径是互斥的所有权模式。

auto 模式在枚举前持有 `/run/ocr-translator-fan.lock`。若专用 `pwm0` 已导出且没有
活跃 owner，则按崩溃遗留接管，退出时 disable/unexport。显式路径下的既有 PWM
会保存并恢复原始 enable/period/duty。

温度升档阈值为 45/55/65/75°C，降档阈值为 42/52/62/72°C，档位占空比为
0/25/50/75/100%。温度读取失败由主程序切到满速。

## 6. 线程安全

- 这些上下文没有通用的内部 pthread mutex。
- sysfs 单次读写并不等于多个线程对上下文状态的原子操作。
- 同一个 light/temp/brightness/fan 对象应由一个控制线程拥有，或由调用方提供外部锁。
- PWM 的 `flock` 只用于跨进程硬件所有权，不替代同一进程内的线程同步。
- 主程序当前每秒执行一次光照、温度和存储状态轮询。

## 7. 返回值

接口通常以 `0` 表示成功，以负值表示参数、发现、文件 I/O、数值范围或硬件状态
错误。不同文件使用不同的细分负值，当前没有统一的 `OCR_ERR_NODEV` 枚举；调用方
应记录失败接口和路径，而不是只解释数字。

## 8. 示例

```c
char light_path[128];
char temp_path[128];
ocr_light_sensor_t light;
ocr_temp_sensor_t temp;
ocr_brightness_t brightness;
ocr_fan_ctrl_t fan;
int value;

if (ocr_iio_find_device("ap3216c",
                        light_path, sizeof(light_path),
                        NULL, 0) == 0)
    ocr_light_sensor_init(&light, light_path);

if (ocr_iio_find_device("adt7410",
                        temp_path, sizeof(temp_path),
                        NULL, 0) == 0)
    ocr_temp_sensor_init(&temp, temp_path);

ocr_brightness_init(&brightness,
                    "/sys/class/backlight/panel-backlight/brightness");
ocr_fan_ctrl_init(&fan, "auto");

if (ocr_light_sensor_read(&light, &value) == 0)
    ocr_brightness_update(&brightness, value);

if (ocr_temp_sensor_read(&temp, &value) == 0)
    ocr_fan_ctrl_update(&fan, value);
else
    ocr_fan_ctrl_set_level(&fan, FAN_MAX_LEVEL);

ocr_fan_ctrl_destroy(&fan);
```

---

> 相关文档：
>
> - [自适应亮度](../02-模块详细设计/08-自适应亮度.md)
> - [温度监测与散热](../02-模块详细设计/09-温度监测与散热.md)
