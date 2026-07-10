# pipeline模块API

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 文件列表](#2-文件列表)
- [3. 数据结构](#3-数据结构)
- [4. 函数API](#4-函数api)
- [5. 线程安全](#5-线程安全)
- [6. 错误码](#6-错误码)
- [7. 示例](#7-示例)

---

## 1. 概述

pipeline 模块管理 V4L2 采集 → RGA 处理 → DRM 显示的零拷贝管线，提供帧的获取与归还接口。

## 2. 文件列表

| 文件          | 说明               |
| ------------- | ------------------ |
| pipeline.h    | 公开 API 声明      |
| pipeline.c    | 实现               |
| pool.c        | DMA-BUF 缓冲池     |

## 3. 数据结构

```c
typedef struct pipeline_ctx pipeline_handle_t;

typedef struct {
    ocr_dmabuf_t *frame;
    uint64_t timestamp;
    int index;
} pipeline_frame_t;

typedef struct {
    int width;
    int height;
    uint32_t format;
    int buf_count;
    int mode;  // 0:实时 1:拍照
} pipeline_config_t;
```

## 4. 函数API

```c
// 创建/销毁
pipeline_handle_t *pipeline_create(const pipeline_config_t *cfg);
void pipeline_destroy(pipeline_handle_t *h);

// 启动/停止
int pipeline_start(pipeline_handle_t *h);
int pipeline_stop(pipeline_handle_t *h);

// 获取帧（阻塞，带超时）
int pipeline_acquire_frame(pipeline_handle_t *h, pipeline_frame_t *frame, int timeout_ms);
int pipeline_release_frame(pipeline_handle_t *h, pipeline_frame_t *frame);

// 模式切换
int pipeline_set_mode(pipeline_handle_t *h, int mode);
```

## 5. 线程安全

- `create/destroy`：非线程安全，主线程调用
- `start/stop`：非线程安全
- `acquire/release_frame`：线程安全，内部加锁
- `set_mode`：线程安全

## 6. 错误码

| 错误码              | 说明               |
| ------------------- | ------------------ |
| OCR_ERR_PARAM       | 配置参数无效       |
| OCR_ERR_NODEV       | V4L2 设备打开失败  |
| OCR_ERR_TIMEOUT     | 帧获取超时         |
| OCR_ERR_NOMEM       | 缓冲分配失败       |

## 7. 示例

```c
pipeline_config_t cfg = {.width=1920, .height=1080, .format=V4L2_PIX_FMT_NV12, .buf_count=4};
pipeline_handle_t *h = pipeline_create(&cfg);
pipeline_start(h);

pipeline_frame_t frame;
pipeline_acquire_frame(h, &frame, 1000);  // 获取帧，超时1s
// 使用 frame.frame->fd ...
pipeline_release_frame(h, &frame);

pipeline_stop(h);
pipeline_destroy(h);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
