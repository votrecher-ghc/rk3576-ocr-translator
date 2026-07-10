# RGA能力与限制

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 支持的格式](#2-支持的格式)
- [3. 支持的操作](#3-支持的操作)
- [4. 限制](#4-限制)
- [5. API 速查](#5-api-速查)

---

## 1. 概述

RGA（Raster Graphic Acceleration Unit）是瑞芯微 2D 图形加速器，支持缩放、旋转、格式转换、裁剪等操作。

## 2. 支持的格式

| 类型       | 格式                                         |
| ---------- | -------------------------------------------- |
| RGB        | RGBA8888, RGB888, RGB565, ARGB8888           |
| YUV 4:2:0  | NV12, NV21, YUV420P                          |
| YUV 4:2:2  | NV16, NV61, YUV422P                          |
| 单色       | Y8                                           |

## 3. 支持的操作

| 操作       | 说明                         |
| ---------- | ---------------------------- |
| 缩放       | 双线性/最近邻                |
| 旋转       | 0/90/180/270 度              |
| 裁剪       | 矩形区域                     |
| 格式转换   | RGB↔YUV                      |
| 镜像       | X/Y 翻转                     |
| 合成       | Alpha 混合                   |

## 4. 限制

| 限制项           | 值                  |
| ---------------- | ------------------- |
| 最大分辨率       | 4096×4096           |
| 最小分辨率       | 2×2                 |
| 对齐要求         | 4 字节              |
| 缩放范围         | 1/16 ~ 16 倍        |
| 并发             | 串行（1 个 RGA）   |

## 5. API 速查

```c
// 封装 DMA-BUF
rga_buffer_t wrapbuffer_fd(int fd, int w, int h, int format);

// 操作
IM_STATUS imresize(rga_buffer_t src, rga_buffer_t dst);
IM_STATUS imcvtcolor(rga_buffer_t src, rga_buffer_t dst);
IM_STATUS imrotate(rga_buffer_t src, rga_buffer_t dst, int angle);
IM_STATUS imcrop(rga_buffer_t src, rga_buffer_t dst, im_rect rect);
IM_STATUS imcomposite(rga_buffer_t src, rga_buffer_t dst, im_rect rect);
```

---

> 相关文档：[04-API接口/04-rga模块API.md](../04-API接口/04-rga模块API.md)
