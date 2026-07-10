# display模块API

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

display 模块基于 DRM/KMS，管理多 Plane 显示与 OSD 叠加，支持 VSync 同步 PageFlip。

## 2. 文件列表

| 文件          | 说明             |
| ------------- | ---------------- |
| display.h     | API 声明         |
| display.c     | DRM/KMS 实现     |
| osd.c         | OSD 文本渲染     |

## 3. 数据结构

```c
typedef struct display_ctx display_handle_t;

typedef enum {
    DISPLAY_LAYER_VIDEO = 0,  // 视频层 NV12
    DISPLAY_LAYER_OSD,        // OSD 层 ARGB
} display_layer_t;

typedef struct {
    int fd;
    int width, height;
    uint32_t format;
} display_buffer_t;
```

## 4. 函数API

```c
display_handle_t *display_create(void);
void display_destroy(display_handle_t *h);

// 图层管理
int display_set_buffer(display_handle_t *h, display_layer_t layer, display_buffer_t *buf);
int display_page_flip(display_handle_t *h);  // 等待VSync

// OSD 文本
int display_osd_draw_text(display_handle_t *h, int x, int y, const char *text, uint32_t color);
int display_osd_clear(display_handle_t *h);

// 亮度
int display_set_brightness(display_handle_t *h, int percent);
```

## 5. 线程安全

- `create/destroy`：非线程安全
- `set_buffer/page_flip`：线程安全（显示线程专用）
- `osd_draw/clear`：线程安全（独立 OSD 缓冲）

## 6. 错误码

| 错误码          | 说明               |
| --------------- | ------------------ |
| OCR_ERR_NODEV   | DRM 设备打开失败   |
| OCR_ERR_IOCTL   | DRM ioctl 失败     |
| OCR_ERR_UNSUPP  | 格式/Plane 不支持  |

## 7. 示例

```c
display_handle_t *disp = display_create();
display_buffer_t buf = {.fd=dmabuf_fd, .width=1280, .height=720, .format=DRM_FORMAT_NV12};
display_set_buffer(disp, DISPLAY_LAYER_VIDEO, &buf);
display_osd_draw_text(disp, 100, 100, "翻译结果", 0xFFFFFFFF);
display_page_flip(disp);  // 提交并等待VSync
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
