# input模块API

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

input 模块通过 evdev 读取按键事件，去抖后分发到主控逻辑。

## 2. 文件列表

| 文件          | 说明           |
| ------------- | -------------- |
| input.h       | API 声明       |
| input.c       | evdev 读取实现 |

## 3. 数据结构

```c
typedef struct input_ctx input_handle_t;

typedef enum {
    INPUT_KEY_CAPTURE = 0,   // 拍照键
    INPUT_KEY_MODE,          // 模式切换键
} input_key_t;

typedef enum {
    INPUT_EVENT_PRESS = 0,
    INPUT_EVENT_RELEASE,
    INPUT_EVENT_LONG_PRESS,
} input_event_type_t;

typedef struct {
    input_key_t key;
    input_event_type_t type;
    uint64_t timestamp;
} input_event_t;

typedef void (*input_callback_t)(const input_event_t *e, void *user);
```

## 4. 函数API

```c
input_handle_t *input_create(const char *evdev_path);
void input_destroy(input_handle_t *h);

int input_start(input_handle_t *h);
int input_stop(input_handle_t *h);

int input_set_callback(input_handle_t *h, input_callback_t cb, void *user);
```

## 5. 线程安全

- `create/destroy`：非线程安全
- `start/stop`：非线程安全
- `set_callback`：非线程安全，启动前设置
- 回调在 input 内部线程执行

## 6. 错误码

| 错误码        | 说明               |
| ------------- | ------------------ |
| OCR_ERR_NODEV | evdev 设备不存在   |
| OCR_ERR_IO    | 读取失败           |

## 7. 示例

```c
void on_key(const input_event_t *e, void *user) {
    if (e->key == INPUT_KEY_CAPTURE && e->type == INPUT_EVENT_PRESS) {
        printf("Capture triggered\n");
    }
}

input_handle_t *in = input_create("/dev/input/event0");
input_set_callback(in, on_key, NULL);
input_start(in);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
