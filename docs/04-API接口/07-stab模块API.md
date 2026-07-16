# stab 模块 API

| 版本 | 日期 | 作者 | 变更说明 |
|---|---|---|---|
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-07-16 | 项目组 | 删除不存在的统一 stab API，对齐 IMU、姿态融合和运动补偿真实接口 |

---

## 1. 概述

防抖不是一个 `stab_create()` 风格的单体对象，而是由以下组件组成：

| 文件 | 职责 |
|---|---|
| `app/stab/imu_reader.h/.c` | 发现 IIO scan layout、独占并读取 triggered buffer |
| `app/stab/attitude_fusion.h/.c` | Madgwick 6 轴姿态融合 |
| `app/stab/motion_compensate.h/.c` | 帧间姿态差到平移/旋转/裁剪参数 |
| `app/isp/rga_stabilizer.h/.c` | 平滑参数并通过 RGA 应用图像补偿 |

当前 roll 补偿是裁剪/平移包络近似，不是任意角仿射旋转。

## 2. 主要数据结构

```c
typedef struct {
    int64_t timestamp;
    int64_t accel_raw[3];
    int64_t gyro_raw[3];
    float   accel_si[3]; /* m/s² */
    float   gyro_si[3];  /* rad/s */
} imu_sample_t;

typedef struct {
    quaternion_t q;
    float beta;
    float sample_dt;
    float max_dt;
} ocr_attitude_t;

typedef struct {
    float dx;
    float dy;
    float angle;
    float scale;
} ocr_stab_params_t;
```

完整字段以对应头文件为准。

## 3. IMU reader API

```c
int ocr_imu_reader_init(ocr_imu_reader_t *reader,
                        const char *dev_path,
                        int buf_depth);
int ocr_imu_reader_start(ocr_imu_reader_t *reader);
int ocr_imu_reader_stop(ocr_imu_reader_t *reader);
int ocr_imu_reader_get(ocr_imu_reader_t *reader, imu_sample_t *sample);
void ocr_imu_reader_destroy(ocr_imu_reader_t *reader);
```

`dev_path` 是 `/dev/iio:deviceX`，但编号必须通过 `name=icm42688` 动态发现。
`get()` 为非阻塞接口：

- `0`：取得一个样本；
- `1`：ringbuffer 当前为空；
- 负值：参数或 reader 状态错误。

reader 会读取 `scan_elements/*_{index,type}` 和 scale，按目标内核实际布局解析数据。
同一 IIO 设备通过 `/run` 进程锁保证只能被一个应用实例启用 buffer。

## 4. 姿态融合 API

```c
int ocr_attitude_init(ocr_attitude_t *att,
                      float sample_hz,
                      float beta);
int ocr_attitude_update(ocr_attitude_t *att,
                        const float accel[3],
                        const float gyro[3],
                        float dt);
int ocr_attitude_update_9dof(ocr_attitude_t *att,
                             const float accel[3],
                             const float gyro[3],
                             const float mag[3],
                             float dt);
void ocr_attitude_to_euler(const ocr_attitude_t *att, euler_t *euler);
void ocr_attitude_reset(ocr_attitude_t *att);
```

`accel` 必须是 m/s²，`gyro` 必须是 rad/s。`dt=0` 时使用初始化采样周期；真实运行
优先使用 IIO timestamp 差值。过大或非有限 `dt` 会返回错误。

## 5. 运动补偿与 RGA API

```c
int ocr_motion_comp_init(ocr_motion_comp_t *mc,
                         float hfov,
                         float vfov,
                         float alpha);
int ocr_motion_comp_update(ocr_motion_comp_t *mc,
                           const ocr_attitude_t *att,
                           uint32_t img_w,
                           uint32_t img_h,
                           ocr_stab_params_t *params);
void ocr_motion_comp_reset(ocr_motion_comp_t *mc);

int ocr_rga_stab_init(ocr_rga_stabilizer_t *stab, float alpha);
int ocr_rga_stab_update(ocr_rga_stabilizer_t *stab,
                        const ocr_stab_params_t *params);
int ocr_rga_stab_apply(ocr_rga_stabilizer_t *stab,
                       ocr_buffer_t *src,
                       ocr_buffer_t *dst);
void ocr_rga_stab_enable(ocr_rga_stabilizer_t *stab, int enable);
```

## 6. 线程安全

- `ocr_imu_reader_t` 自己拥有读取线程和 ringbuffer。
- `init/start/stop/destroy` 不应被多个线程并发调用。
- `ocr_imu_reader_get()` 设计为单消费者；增加多个消费者前需重新定义样本归属。
- `ocr_attitude_t`、`ocr_motion_comp_t` 和 `ocr_rga_stabilizer_t` 是普通可变上下文，
  没有内部互斥；由调用方保证单线程访问或外部同步。
- 主程序在显示管线线程中消费样本并更新补偿参数。

## 7. 示例

```c
char imu_sysfs[256];
char imu_dev[64];
ocr_imu_reader_t reader;
ocr_attitude_t attitude;
imu_sample_t sample;

if (ocr_iio_find_device("icm42688",
                        imu_sysfs, sizeof(imu_sysfs),
                        imu_dev, sizeof(imu_dev)) != 0)
    return -1;

if (ocr_imu_reader_init(&reader, imu_dev, 1024) != 0)
    return -1;
if (ocr_attitude_init(&attitude, 1000.0f, 0.1f) != 0)
    return -1;
if (ocr_imu_reader_start(&reader) != 0)
    return -1;

while (ocr_imu_reader_get(&reader, &sample) == 0) {
    /* 实际代码使用相邻 sample.timestamp 计算 dt。 */
    ocr_attitude_update(&attitude,
                        sample.accel_si,
                        sample.gyro_si,
                        0.0f);
}

ocr_imu_reader_stop(&reader);
ocr_imu_reader_destroy(&reader);
```

---

> 相关文档：
>
> - [电子防抖设计](../02-模块详细设计/06-电子防抖.md)
> - [ICM42688 驱动开发详解](../03-驱动开发/05-ICM42688驱动开发详解.md)
