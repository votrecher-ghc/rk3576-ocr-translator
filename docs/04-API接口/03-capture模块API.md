# capture模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |
| v2.0 | 2026-10-26 | 项目组 | 按实际实现重写为 V4L2 采集 API；按键拍照归档移至 input 模块 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 文件列表](#2-文件列表)
- [3. 函数API](#3-函数api)
- [4. 帧的所有权](#4-帧的所有权)
- [5. 线程安全](#5-线程安全)
- [6. 示例](#6-示例)

---

## 1. 概述

capture 模块负责摄像头采集：以 V4L2 MMAP 方式申请缓冲，导出为 DMA-BUF，由自带的采集线程持续取帧，交给管线并保留最新一帧供他人借用。

按键拍照与归档不在本模块，见 [09-input模块API.md](09-input模块API.md)。拍照复用实时识别的结果，不单独采集高分辨率帧。

## 2. 文件列表

| 文件 | 说明 |
| ---- | ---- |
| `v4l2_capture.h/.c` | 打开设备、协商格式、申请并导出缓冲、采集线程、最新帧缓存 |
| `v4l2_dmabuf.h/.c` | `VIDIOC_EXPBUF` 封装 |
| `imx415_ctrl.h/.c` | 曝光、增益、自动曝光等 V4L2 控件；借用采集设备 fd |

## 3. 函数API

```c
typedef int (*ocr_v4l2_frame_cb_t)(ocr_buffer_t *buffer, void *user_data);

int ocr_v4l2_open_ex(ocr_v4l2_capture_t *cap, const char *dev_name,
                     uint32_t width, uint32_t height, uint32_t fps,
                     ocr_pixel_format_t format, int buffer_count);
int ocr_v4l2_open(ocr_v4l2_capture_t *cap, const char *dev_name,
                  uint32_t width, uint32_t height, uint32_t fps,
                  ocr_pixel_format_t format);          /* buffer_count = 4 */
int ocr_v4l2_set_frame_callback(ocr_v4l2_capture_t *cap,
                                ocr_v4l2_frame_cb_t callback, void *user_data);
int ocr_v4l2_start(ocr_v4l2_capture_t *cap);
ocr_buffer_t *ocr_v4l2_get_latest(ocr_v4l2_capture_t *cap);
int ocr_v4l2_stop(ocr_v4l2_capture_t *cap);
int ocr_v4l2_close(ocr_v4l2_capture_t *cap);
int ocr_v4l2_set_fps(ocr_v4l2_capture_t *cap, uint32_t fps);

int ocr_v4l2_export_dmabuf_ex(int v4l2_fd, uint32_t buffer_type,
                              int index, uint32_t plane);

int ocr_imx415_init(ocr_imx415_t *imx, int v4l2_fd);
int ocr_imx415_set_ae(ocr_imx415_t *imx, int enable);
int ocr_imx415_set_exposure(ocr_imx415_t *imx, int32_t exposure);
int ocr_imx415_set_gain(ocr_imx415_t *imx, int32_t gain);
```

| 函数 | 说明 |
| ---- | ---- |
| `open_ex` | `QUERYCAP`（单平面或 MPLANE）→ `S_FMT`（回读实际尺寸与 stride）→ `S_PARM` → `REQBUFS(MMAP)` → 每块 `QUERYBUF` + `mmap` + `EXPBUF`，注册进采集池；格式支持 NV12/NV16/YUYV，缓冲 2..8 块；失败返回 `-2` 并清理 |
| `set_frame_callback` | 只能在 `start` 前设置；回调在采集线程中执行 |
| `start` | 全部缓冲空闲时才能启动：全部 `QBUF` → `STREAMON` → 创建采集线程；有块仍被引用返回 `-3` |
| `get_latest` | 返回最新帧的一份新引用，用完必须 `ocr_buffer_unref`；可能为 NULL |
| `stop` | `STREAMOFF`，join 采集线程（最多等 1 s 的 select 超时），释放最新帧缓存 |
| `close` | 先 stop，再销毁采集池；仍有块被引用时返回 `-2` 并保留资源，可重试 |
| `imx415_init` | 查询曝光、增益范围；控件不存在时使用默认上限，仍返回 0 |
| `imx415_set_ae` | 设置 `V4L2_CID_EXPOSURE_AUTO`；主程序忽略失败 |

`ocr_imx415_set_resolution` 是未实现的占位函数，返回 `-ENOTSUP`。

## 4. 帧的所有权

- 采集线程 `DQBUF` 后用 `ocr_pool_acquire_index` 取得该块（引用计数 1），校验错误标志与有效长度，不合格的帧直接释放、立即重新 `QBUF`。
- 时间戳：驱动给出单调时间戳时使用驱动值，否则用取帧时刻的 `CLOCK_MONOTONIC`；`frame_id` 递增。
- 每帧额外加一份引用存为 `latest_frame`，替换下来的旧帧在锁外释放。
- 帧回调返回 0 表示接管了那份引用；返回非 0 时由采集线程释放。
- 一块缓冲的最后一个引用释放时，`capture_recycle` 在 `io_lock` 内 `QBUF`，把它还给驱动；`STREAMOFF` 之后归还的块不再 `QBUF`。
- `QBUF` 失败会置 `queue_error` 并让采集线程退出，主循环据此以退出码 10 结束进程。

## 5. 线程安全

- `open/start/stop/close` 由主线程调用，不应并发。
- `io_lock` 串行化 `QBUF`、`DQBUF` 和 `STREAMON/OFF`；`latest_lock` 保护 `latest_frame` 指针。锁顺序为：pool.lock → io_lock；释放旧的最新帧时不持有 `latest_lock`。
- `get_latest` 可由任意线程调用。

## 6. 示例

```c
ocr_v4l2_capture_t cap;

if (ocr_v4l2_open_ex(&cap, "/dev/video0", 1920, 1080, 30, OCR_FMT_NV12, 4) != 0)
    return -1;
ocr_v4l2_set_frame_callback(&cap, capture_frame, &rga_node);
ocr_v4l2_start(&cap);

/* 任意线程借用最新帧 */
ocr_buffer_t *latest = ocr_v4l2_get_latest(&cap);
if (latest) {
    /* 读 latest->frame_id 等 */
    ocr_buffer_unref(latest);
}

ocr_v4l2_stop(&cap);
ocr_v4l2_close(&cap);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)、[../02-模块详细设计/03-摄像头与零拷贝链路.md](../02-模块详细设计/03-摄像头与零拷贝链路.md)
