# RK3576 OCR 翻译系统——实现与交接指南

> **最后更新日期：2026-07-16**
>
> **文档版本：v2.0**
>
> **交接基线：** 本轮基于 GitHub `main` 的初始骨架提交 `e039fb1` 继续实现。
>
> **当前结论：** 应用主链、外设用户态逻辑、自研驱动、构建部署入口和离线测试已经从骨架补齐；下一阶段必须在与目标镜像同源的野火/Rockchip RK3576 SDK、真实模型资产和鲁班猫3实板上完成交叉编译与硬件验收。未完成这些步骤前，不得标记为量产验收通过。

本文是后续开发的主交接入口。更细的真实完成度和板端通过条件见：

- `docs/07-测试/08-当前实现状态与板端验收清单.md`
- `docs/05-编译构建/09-SDK到板卡完整构建指南.md`
- `README.md`

---

## 一、2026-07-16 当前状态

### 1.1 总体完成度

| 范围 | 当前状态 | 交接说明 |
|---|---|---|
| utils / comm | 已完成代码闭环 | 日志、配置、时间戳、线程辅助、ringbuffer、msgbus、event loop 均有生产实现和硬件无关测试 |
| pipeline | 已完成代码闭环 | 引用计数、缓冲池、节点队列、停止/失败传播、DMA-HEAP 与 DMA-BUF 描述已实现 |
| capture / RGA / DRM | 已完成实现，待板端验证 | V4L2 MMAP + DMA-BUF、RGA 处理、DRM/KMS page-flip 与文字 overlay 已接通 |
| OCR / 翻译 | 已完成运行时实现，缺真实模型验收 | RKNN 张量契约、OCR 检测/识别/后处理、静态 encoder/decoder 翻译链已实现 |
| 防抖 | 已完成实现，待画质和性能验收 | ICM42688 IIO buffer、姿态融合、裁剪/平移补偿已接入；真实任意角 roll 仍受 RGA 接口限制 |
| sensors / input / archive | 已完成实现，待外设验收 | IIO/按键动态发现、背光、PWM 风扇、拍照会话、归档和安全清理已实现 |
| main / 生命周期 | 已完成实现 | live/photo 模式、信号退出、看门狗、故障码和依赖顺序清理已闭合 |
| BSP / Buildroot / 部署 | 已完成项目侧入口，待 vendor SDK 验证 | external tree、驱动/overlay 构建、rootfs 后处理、镜像和安全部署脚本已补齐 |
| 测试 | 离线静态验收通过，缺目标编译和实板测试 | 本机没有 RK3576 编译器、sysroot、RGA/RKNN SDK 和硬件 |
| 文档 | 已按当前实现更新 | 旧的固定 IIO/input/PWM 编号、错误 compatible、错误传感器比例和未落地策略已清理 |

本轮预计提交范围为：

- 120+ 个已有版本文件修改；
- 20+ 个新增文件；
- 覆盖应用、驱动、Buildroot、脚本、配置、测试和文档。

### 1.2 状态定义

- **已完成代码闭环**：本项目侧实现、错误处理和资源回收已经落地，并通过当前环境可执行的静态检查。
- **待 SDK 验证**：需要 vendor kernel/sysroot/库完成真实 AArch64 编译。
- **待板端验收**：需要真实引脚、供电、media graph、DRM plane、PWM 极性和外设数据验证。
- **外部资产阻塞**：仓库不包含可分发的 ONNX/RKNN、词表、量化图片和目标 CJK 字体。

---

## 二、本轮已完成内容

### 2.1 基础设施与数据管线

- 完成线程安全日志、嵌套 JSON 配置、路径长度校验和参数范围检查。
- 完成 SPSC ringbuffer、eventfd 消息总线和 epoll 事件循环。
- 完成带原子引用计数的 `ocr_buffer_t`、固定缓冲池、阻塞队列和可重试停止流程。
- 新增 DMA-HEAP 分配器，维护 fd、stride、plane offset、映射状态和 CPU/DMA 同步边界。
- 管线节点会传播处理错误；停止时唤醒等待线程，避免退出阶段永久阻塞。

### 2.2 V4L2、RGA 与 DRM/KMS

- V4L2 支持格式协商、MMAP 缓冲、`VIDIOC_EXPBUF`、全量 QBUF、STREAMON/OFF 和采集线程。
- 采集缓冲按索引绑定，最后一个引用归还后才重新 QBUF，避免设备覆盖仍在使用的帧。
- 支持单平面或单个连续 DMA-BUF 中的多平面描述；明确拒绝当前实现无法安全处理的独立多 fd plane 和非零 data offset。
- RGA 封装支持 DMA-BUF resize、crop、format convert，以及防抖裁剪/平移路径。
- DRM 会选择有效 connector/CRTC/plane，导入 DMA-BUF framebuffer，并等待 page-flip 完成后释放上一帧。
- ARGB 文字 overlay 使用双缓冲，退出时恢复被应用修改的 KMS 状态。

### 2.3 RKNN OCR 与翻译

- 模型加载器会检查文件、大小、上下文创建和输入输出张量属性。
- OCR 检测支持预处理、概率图后处理、连通区域、旋转四点框和坐标回映。
- OCR 识别支持四点区域透视采样、动态宽度预处理、UTF-8 词表和 CTC 去重/blank 解码。
- 翻译只接受项目定义的静态双模型契约：
  - `greedy-vocab-v1`
  - encoder/decoder 分离
  - 固定最大序列长度
  - greedy 自回归解码
- live 和 photo 路径不会并发使用同一 RKNN 上下文。
- 模型构建会生成 `model_artifacts.sha256`；rootfs 构建会再次验证模型、词表与 manifest。

### 2.4 防抖与 ICM42688

- 修正 ICM42688-P 寄存器、WHO_AM_I、量程、比例和温度换算。
- 使用 INT1 数据就绪中断驱动 IIO trigger/pollfunc，不再以伪 buffer 代替。
- 固定 scan mask 为温度、三轴加速度、三轴陀螺仪和 soft timestamp。
- 当前配置为 ±4g、±2000dps、1kHz ODR；默认应用融合采样率同步为 1kHz。
- 用户态读取 `scan_elements/*_{index,type}` 推导布局，不把 IIO 帧结构写死。
- IMU buffer 使用进程锁避免多个实例同时启用；异常退出后的 stale enable 会在持锁后清理。
- Madgwick 姿态融合使用样本时间戳计算 `dt`，异常大间隔会被拒绝。

### 2.5 AP3216C、ADT7410、按键与 PWM 风扇

- AP3216C 按 Rev0.86 数据手册修正：
  - PS 配置/LED/数据寄存器地址；
  - 10-bit PS 解码；
  - `IR_OF` 溢出返回错误；
  - ALS 默认档 `0.35 lux/count`；
  - 软件 W1C 中断清除；
  - Linux 6.1 `.probe_new`/`void remove`。
- ADT7410 使用精确的 `125/16 = 7.8125 m°C/LSB`，同时提供：
  - `in_temp_input`
  - `in_temp_raw`
  - `in_temp_scale`
  - `in_temp_offset`
- 用户态温度回退严格使用 `(raw + offset) * scale`，不再按数值大小猜测单位。
- 光照 raw 回退必须成对读取对应 scale，不再把 ADC count 当作 lux。
- IIO 设备按 sysfs `name` 唯一匹配；重复名称会明确失败。
- input 设备按 `EV_KEY + KEY_CAMERA` capability 唯一匹配，不依赖 `event0`。
- 风扇 auto 模式通过 DT 的 `ocr,fan-pwm` 标记定位动态编号的 `pwmchip`。
- 风扇控制在任何 PWM 枚举或写入前获取 `/run/ocr-translator-fan.lock` 非阻塞独占锁。
- auto 模式会接管并清理崩溃遗留的专用 `pwm0`；显式路径模式则保存和恢复外部 PWM 状态。
- 温度缺失或读取失败时切到满速故障安全档。
- 默认 overlay 只启用 PWM controller，不创建第二个内核 consumer。

### 2.6 输入、拍照和归档

- KEY_CAMERA 线程区分初始化、运行、线程启动和工作线程错误状态。
- 拍照会话与 live OCR 互斥访问推理资源。
- JPEG 当前使用 libjpeg CPU 编码，DMA CPU 同步区间完整配平。
- 归档按时间戳写入图片、文本和 JSON。
- 存储清理由已打开的根目录 fd 锚定，使用 `fstatat`/`unlinkat` 且拒绝符号链接逃逸。

### 2.7 主程序与资源生命周期

- 主程序按配置初始化 capture、DRM、AI、防抖、传感器、风扇、按键和归档。
- 看门狗会检测采集停滞、按键工作线程死亡和管线异常。
- live/photo 模式均有明确退出码。
- 退出按依赖逆序清理；如果某个资源仍处于活动状态，不继续销毁它依赖的对象。
- PWM 在其他管线资源之前恢复，避免后续清理提前返回时遗留风扇状态。

### 2.8 构建、模型与部署

- 新增 `scripts/build/`：
  - `build_uboot.sh`
  - `build_kernel.sh`
  - `build_app.sh`
  - `build_models.sh`
  - `build_rootfs.sh`
  - `make_image.sh`
- 应用交叉构建强制使用目标 sysroot，并隔离宿主机 pkg-config/include/library。
- 可通过以下显式变量提供 vendor 依赖：
  - `OCR_RGA_INCLUDE_DIR`
  - `OCR_RGA_LIBRARY`
  - `OCR_RKNN_INCLUDE_DIR`
  - `OCR_RKNN_LIBRARY`
- 内核构建会检查所需 Kconfig，并验证 5 个模块已安装到模块归档。
- Buildroot external tree 已补齐；post-build 会验证 AArch64 ELF、目标库、模型 checksum 和字体。
- overlay 只允许二选一启用：
  - `DT_OVERLAY_MODE=merged`
  - `DT_OVERLAY_MODE=vendor-resource`
- 未核对 media graph 的 IMX415 overlay 默认不构建，需显式设置 `BUILD_UNVERIFIED_CAMERA_OVERLAY=1`。
- 烧录脚本没有危险的默认块设备；完整 `update.img` 必须调用真实 vendor 打包工具。

### 2.9 测试与文档

- 生产源码已接入以下硬件无关测试：
  - buffer / buffer pool
  - ringbuffer
  - msgbus
  - event loop
  - config
  - pipeline
  - OCR post-processing
- 更新了 SDK 构建指南、驱动文档、IIO 用户态文档、散热文档和部署诊断文档。
- 移除或纠正了以下旧内容：
  - 固定 `iio:device0/1/2`
  - 固定 `/dev/input/event0`
  - 固定 `pwmchip9`
  - 旧 `lbc,ap3216c` / `inv,icm42688` / `adi,adt7410` compatible
  - AP3216C 错误的 `35271` scale
  - 与当前实现不符的 20kHz/50000ns 风扇参数
  - 尚未实现却写成已有功能的 NPU 降频和自动关机策略

---

## 三、关键运行设计与不可回退约束

### 3.1 设备编号不能写死

运行时只允许：

- IIO：按 `name` 发现 `ap3216c`、`adt7410`、`icm42688`；
- input：按 `KEY_CAMERA` capability 发现；
- PWM：按 DT 的 `ocr,fan-pwm` marker 发现；
- V4L2/DRM：仍由主配置显式指定，因为一个板上可能存在多个合法视频或显示设备。

不要重新引入 `iio:device0`、`event0` 或 `pwmchipN` 作为默认运行时假设。

### 3.2 一个 PWM 通道只能有一个 owner

当前默认架构是应用直接控制 PWM sysfs：

- DTS 只启用 PWM controller 并添加 marker；
- 不创建 `pwm-fan` 或 `ocr,pwm-fan` consumer；
- 不允许另一个服务同时写该 PWM；
- 应用退出失败时保留锁和所有权状态，以便重试清理。

如改为内核 PWM consumer，必须同时禁用应用的 sysfs 风扇控制。

### 3.3 overlay 启用路径互斥

`merged` 与 `vendor-resource` 不能同时使用，否则可能重复实例化同一设备。默认构建和 TFTP 只使用 vendor base DTB，只有完成原理图/base DTS 核对后才启用项目 overlay。

### 3.4 只使用目标镜像同源 SDK

不要使用宿主机或任意上游 Buildroot 的 RGA/RKNN 库替代 vendor SDK。以下内容必须同源：

- kernel headers 与内核配置；
- sysroot；
- librga；
- librknnrt；
- RKNN Toolkit2；
- 板端 NPU 驱动；
- Buildroot package 定义。

### 3.5 当前功能边界

- JPEG 是 CPU libjpeg 路径，不是 MPP 零拷贝硬编码。
- roll 防抖是裁剪/平移包络近似，不是真实任意角仿射。
- OCR 预处理存在 CPU fallback。
- 翻译只支持当前静态 greedy 双模型契约。
- 仓库不提供真实模型、词表、量化图片或产品字体。

---

## 四、当前构建入口

### 4.1 准备 vendor SDK

在 Ubuntu 20.04/22.04 x86_64 主机设置：

```bash
export UBOOT_SRC=/path/to/vendor-sdk/u-boot
export KERNEL_SRC=/path/to/vendor-sdk/kernel-6.1
export BUILDROOT_SRC=/path/to/vendor-sdk/buildroot
export SYSROOT=/path/to/vendor-sdk/sysroot
export CROSS_COMPILE=aarch64-buildroot-linux-gnu-
```

实际目录、工具链前缀、defconfig 和 DTB 名称必须从目标 SDK 获取，不要照抄占位路径。

### 4.2 构建顺序

```bash
bash scripts/build/build_uboot.sh
bash scripts/build/build_kernel.sh
bash scripts/build/build_app.sh
bash scripts/build/build_models.sh
bash scripts/build/build_rootfs.sh
bash scripts/build/make_image.sh
```

### 4.3 模型输入

```bash
export DET_ONNX_PATH=/path/to/det.onnx
export DET_QUANT_IMG_DIR=/path/to/calibration_images
export REC_ONNX_PATH=/path/to/rec.onnx
export OCR_REC_VOCAB_PATH=/path/to/ppocr_keys_v1.txt

export TRANS_ENCODER_ONNX_PATH=/path/to/encoder.onnx
export TRANS_DECODER_ONNX_PATH=/path/to/decoder.onnx
export TRANS_SRC_VOCAB_PATH=/path/to/source_vocab.txt
export TRANS_TGT_VOCAB_PATH=/path/to/target_vocab.txt
export TRANS_TOKENIZER_CONTRACT=greedy-vocab-v1
export TRANS_SRC_LANG=en
export TRANS_TGT_LANG=zh
```

转换完成后必须保留并部署：

```text
output/models/model_artifacts.sha256
```

### 4.4 overlay 打包

```bash
export OCR_DTBO_LIST="ocr-ap3216c.dtbo ocr-adt7410.dtbo ocr-pwm-fan.dtbo"
export DT_OVERLAY_MODE=merged
bash scripts/build/make_image.sh
```

若 vendor 固件必须使用 resource 打包：

```bash
export DT_OVERLAY_MODE=vendor-resource
export RK_PACK_SCRIPT=/path/to/vendor/pack-script
bash scripts/build/make_image.sh
```

两种模式只能选择一种。

---

## 五、本轮离线校验结果

截至 2026-07-16，当前 Windows 工作环境完成以下检查：

| 检查 | 结果 |
|---|---|
| `git diff --check` | 通过 |
| JSON 配置解析 | 3/3 通过 |
| 模型转换脚本 Python AST | 3/3 通过 |
| Shell/启动脚本 `sh -n` | 14/14 通过 |
| `CMakeLists.txt` | 14 个可读取 |
| 交叉工具链 CMake | 1 个可读取 |
| CMake 直接 C/C++ 源文件引用 | 53/53 存在 |
| 生产源码 TODO/FIXME 扫描 | 0 个遗留 |
| 运行时固定 IIO/input/PWM 编号扫描 | 无生效配置遗留 |
| PWM overlay 双 owner 检查 | 通过 |

这些检查不能替代：

- AArch64 应用编译；
- Linux 6.1 内核模块编译；
- `dtc`/vendor overlay 打包；
- RKNN 模型转换与 golden-vector；
- 实板 V4L2/RGA/DRM/IIO/PWM 联调；
- 长时间稳定性和温升测试。

---

## 六、后续必须提供的外部条件

继续推进前需要具备：

1. LubanCat/Rockchip RK3576 vendor SDK 的实际解压目录；
2. 与目标 rootfs 同源的 AArch64 sysroot 和交叉编译器；
3. vendor `librga`、`librknnrt`、头文件及 Buildroot package；
4. 鲁班猫3原理图、40Pin 引脚表和实际 base DTS；
5. 实板、串口、摄像头、屏幕和所有外设；
6. 真实 ONNX/RKNN、词表、量化图片和允许部署的 CJK 字体；
7. 产品目标：FPS、P95 延迟、温升、功耗和允许的 OCR/翻译误差。

---

## 七、建议板端验收顺序

1. **只编译，不启用 overlay**
   - 构建应用、内核模块和 base DTB；
   - 检查 AArch64 ELF、动态依赖和内核符号。
2. **逐个核对硬件资源**
   - I2C/SPI controller；
   - GPIO bank/pin；
   - IRQ 极性；
   - pinctrl；
   - regulator；
   - PWM 通道与极性。
3. **逐个启用外设**
   - AP3216C；
   - ADT7410；
   - ICM42688；
   - KEY_CAMERA；
   - PWM 风扇。
4. **验证显示和摄像头**
   - `media-ctl -p`
   - `v4l2-ctl`
   - `modetest`
   - RGA DMA-BUF 格式/stride
5. **验证模型**
   - toolkit/runtime/driver 版本；
   - manifest 与 checksum；
   - OCR/翻译 golden vectors。
6. **启动应用**
   - live 模式；
   - photo 模式；
   - SIGTERM；
   - 摄像头断流；
   - 温度读取失败；
   - 磁盘高水位；
   - 模型缺失。
7. **长稳与性能**
   - 至少 30 分钟无泄漏、冻屏或缓冲耗尽；
   - 记录 FPS、P50/P95 延迟、CPU/NPU/RGA 利用率；
   - 验证风扇升降档、异常退出和重启恢复。

详细通过条件填写到：

```text
docs/07-测试/08-当前实现状态与板端验收清单.md
docs/07-测试/07-测试报告模板.md
```

---

## 八、已知风险与待办

| 风险/待办 | 当前处理 |
|---|---|
| vendor Buildroot 的实际 RGA/RKNN symbol 可能不同 | 构建脚本显式失败；需要在实际 SDK 中核对 |
| 板级 defconfig、base DTB 和 overlay 机制未知 | 默认不自动启用 overlay |
| GPIO/PWM/I2C/SPI 引脚仍未按实板闭环 | 文档和 overlay 均标记为必须核对 |
| IMX415 media graph 未验证 | overlay 默认跳过，优先沿用出厂 DTS |
| RKNN 模型 shape/量化结果未知 | 依靠严格 manifest 和 tensor contract 阻止静默错配 |
| CPU JPEG 可能影响拍照延迟 | 板端正确性通过后再接 MPP DMA-BUF 编码 |
| 防抖不支持真实任意角旋转 | 产品要求明确后选择 RGA 仿射/透视或其他实现 |
| GitHub CI 尚未建立 | 后续可把硬件无关 CTest、Shell/JSON/CMake 检查接入 CI |

---

## 九、关键文件索引

| 文件/目录 | 用途 |
|---|---|
| `README.md` | 项目入口与快速构建 |
| `app/main/main.c` | 主流程和生命周期 |
| `app/CMakeLists.txt` | 应用依赖与硬件/测试构建开关 |
| `app/pipeline/` | 缓冲、DMA-HEAP、节点和管线 |
| `app/capture/` | V4L2 与 DMA-BUF |
| `app/display/` | DRM/KMS 与 overlay |
| `app/ai/` | RKNN、OCR 与翻译 |
| `app/stab/` | IIO IMU、姿态融合与补偿 |
| `app/sensors/` | IIO 发现、亮度、温度和 PWM 风扇 |
| `bsp/kernel/drivers/` | 自研 Linux 6.1 驱动 |
| `bsp/kernel/arch/arm64/boot/dts/rockchip/overlays/` | 待板级核对的 overlay |
| `bsp/buildroot/` | Buildroot external tree 与 rootfs overlay |
| `scripts/build/` | 构建入口 |
| `scripts/deploy/` | 安全部署入口 |
| `models/convert/` | RKNN 转换入口 |
| `tests/` | 硬件无关测试与集成测试 |
| `docs/05-编译构建/09-SDK到板卡完整构建指南.md` | SDK 到实板构建细节 |
| `docs/07-测试/08-当前实现状态与板端验收清单.md` | 当前真实边界和验收标准 |

---

## 十、后续开发规则

1. 不要把静态检查通过描述成已完成目标机编译或实板验收。
2. 不要重新引入固定 IIO/input/PWM 编号。
3. 不要让内核 PWM consumer 与应用 sysfs owner 同时控制一个通道。
4. 不要混用宿主机、上游 Buildroot 与 vendor SDK 的 RGA/RKNN 依赖。
5. 不要在未核对原理图和 base DTS 前默认合入项目 overlay。
6. 模型、词表和 manifest 必须作为一个带 checksum 的部署单元。
7. 资源销毁失败时保留依赖对象，优先让失败可诊断、可重试。
8. 每次改变板端契约后，同步更新本文件和板端验收清单，并标记日期。

---

## 十一、文档变更记录

| 版本 | 日期 | 说明 |
|---|---|---|
| v1.0 | 2026-07-10 | 建立项目骨架阶段的实现交接说明。 |
| v2.0 | 2026-07-16 | 按当前代码重写完成度、构建部署入口、运行约束、外部阻塞项和板端验收顺序；记录本轮应用、驱动、测试及文档修改。 |

---

> **2026-07-16 交接结论：** 本项目已经不再是“待填充的代码骨架”。下一位开发者应从 vendor SDK 交叉编译和实板验收开始，而不是重新实现应用主链。
