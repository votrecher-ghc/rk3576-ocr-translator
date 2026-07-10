# comm模块API

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

comm 模块负责配置文件管理、运行时参数调整与状态查询，提供模块间共享的配置服务。

## 2. 文件列表

| 文件          | 说明               |
| ------------- | ------------------ |
| comm.h        | API 声明           |
| config.c      | JSON 配置解析      |
| state.c       | 状态管理           |

## 3. 数据结构

```c
typedef struct comm_ctx comm_handle_t;

typedef struct {
    int preview_fps;
    int ocr_interval_ms;
    int brightness;
    int fan_mode;        // 0:自动 1:手动
    int fan_duty;
    char src_lang[8];
    char dst_lang[8];
    int eis_enable;
    int log_level;
} comm_config_t;

typedef struct {
    int mode;            // 0:实时 1:拍照
    int fps;
    int temp_mc;
    int cpu_usage;
    size_t mem_used;
} comm_state_t;
```

## 4. 函数API

```c
comm_handle_t *comm_create(const char *config_path);
void comm_destroy(comm_handle_t *h);

// 配置
int comm_load_config(comm_handle_t *h, const char *path);
int comm_save_config(comm_handle_t *h);
int comm_get_config(comm_handle_t *h, comm_config_t *cfg);
int comm_set_config(comm_handle_t *h, const comm_config_t *cfg);

// 状态
int comm_get_state(comm_handle_t *h, comm_state_t *state);

// 配置变更通知
typedef void (*comm_config_cb_t)(const comm_config_t *cfg, void *user);
int comm_set_config_callback(comm_handle_t *h, comm_config_cb_t cb, void *user);
```

## 5. 线程安全

- 所有函数线程安全
- 配置读写使用读写锁（多读单写）
- 回调在配置变更线程执行

## 6. 错误码

| 错误码        | 说明             |
| ------------- | ---------------- |
| OCR_ERR_IO    | 配置文件读写失败 |
| OCR_ERR_PARAM | JSON 格式错误    |

## 7. 示例

```c
comm_handle_t *c = comm_create("/ocr/conf/config.json");
comm_config_t cfg;
comm_get_config(c, &cfg);
cfg.brightness = 80;
comm_set_config(c, &cfg);
comm_save_config(c);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
