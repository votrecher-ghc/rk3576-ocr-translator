# rga模块API

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

rga 模块封装 librga，提供图像缩放、旋转、格式转换的硬件加速接口，支持 DMA-BUF fd 输入输出。

## 2. 文件列表

| 文件            | 说明           |
| --------------- | -------------- |
| rga_wrapper.h   | API 声明       |
| rga_wrapper.c   | RGA 封装实现   |

## 3. 数据结构

```c
typedef struct {
    int fd;
    int width, height;
    int format;  // RK_FORMAT_*
} rga_buffer_t;

typedef struct {
    int x, y, w, h;
} rga_rect_t;
```

## 4. 函数API

```c
// 缓冲管理
rga_buffer_t rga_wrap_fd(int fd, int w, int h, int format);
int rga_alloc_buffer(int w, int h, int format, rga_buffer_t *buf);
int rga_free_buffer(rga_buffer_t *buf);

// 图像处理
int rga_resize(rga_buffer_t *src, rga_buffer_t *dst, rga_rect_t *src_rect, rga_rect_t *dst_rect);
int rga_cvtcolor(rga_buffer_t *src, rga_buffer_t *dst);
int rga_rotate(rga_buffer_t *src, rga_buffer_t *dst, int angle);
int rga_blit(rga_buffer_t *src, rga_buffer_t *dst, rga_rect_t *src_rect, rga_rect_t *dst_rect, int rotate);
```

## 5. 线程安全

- 所有函数线程安全，RGA 驱动内部排队
- 但同一 `rga_buffer_t` 不建议并发处理

## 6. 错误码

| 错误码          | 说明               |
| --------------- | ------------------ |
| OCR_ERR_PARAM   | 参数/格式不支持     |
| OCR_ERR_IOCTL   | RGA ioctl 失败     |
| OCR_ERR_NOMEM   | 缓冲分配失败       |

## 7. 示例

```c
rga_buffer_t src = rga_wrap_fd(v4l2_fd, 1920, 1080, RK_FORMAT_YCbCr_420_SP);
rga_buffer_t dst;
rga_alloc_buffer(1280, 720, RK_FORMAT_YCbCr_420_SP, &dst);

rga_rect_t s = {0, 0, 1920, 1080}, d = {0, 0, 1280, 720};
rga_resize(&src, &dst, &s, &d);

rga_free_buffer(&dst);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
