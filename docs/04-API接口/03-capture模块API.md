# capture模块API

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

capture 模块负责按键拍照模式的高分辨率帧捕获与结果合成。

## 2. 文件列表

| 文件          | 说明           |
| ------------- | -------------- |
| capture.h     | API 声明       |
| capture.c     | 拍照流程实现   |

## 3. 数据结构

```c
typedef struct capture_ctx capture_handle_t;

typedef struct {
    char jpg_path[256];
    char txt_path[256];
    char text[2048];
    uint64_t timestamp;
} capture_result_t;

typedef void (*capture_callback_t)(const capture_result_t *result, void *user);
```

## 4. 函数API

```c
capture_handle_t *capture_create(pipeline_handle_t *pipe, ai_handle_t *ai);
void capture_destroy(capture_handle_t *h);

// 触发拍照（异步）
int capture_trigger(capture_handle_t *h, capture_callback_t cb, void *user);

// 查询状态
int capture_get_state(capture_handle_t *h);  // 0:空闲 1:拍照中
```

## 5. 线程安全

- `create/destroy`：非线程安全
- `trigger`：线程安全，可从任意线程调用
- `get_state`：线程安全

## 6. 错误码

| 错误码        | 说明           |
| ------------- | -------------- |
| OCR_ERR_BUSY  | 正在拍照中     |
| OCR_ERR_NPU   | OCR 推理失败   |
| OCR_ERR_IO    | 文件保存失败   |

## 7. 示例

```c
void on_captured(const capture_result_t *r, void *user) {
    printf("Saved: %s\n", r->jpg_path);
}

capture_handle_t *cap = capture_create(pipe, ai);
capture_trigger(cap, on_captured, NULL);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
