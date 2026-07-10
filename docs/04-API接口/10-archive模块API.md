# archive模块API

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

archive 模块负责拍照翻译结果的归档存储、查询与清理管理。

## 2. 文件列表

| 文件          | 说明               |
| ------------- | ------------------ |
| archive.h     | API 声明           |
| archive.c     | 归档存储实现       |

## 3. 数据结构

```c
typedef struct archive_ctx archive_handle_t;

typedef struct {
    char id[32];          // 时间戳ID
    char jpg_path[256];
    char txt_path[256];
    char text[2048];
    uint64_t timestamp;
    size_t size;
} archive_item_t;

typedef struct {
    uint64_t start_ts;    // 0表示不限
    uint64_t end_ts;
    int limit;
    int offset;
} archive_query_t;
```

## 4. 函数API

```c
archive_handle_t *archive_create(const char *base_dir);
void archive_destroy(archive_handle_t *h);

// 保存
int archive_save(archive_handle_t *h, const char *jpg_path,
                 const char *text, const char *translated, archive_item_t *out);

// 查询
int archive_query(archive_handle_t *h, const archive_query_t *q,
                  archive_item_t *items, int max_count, int *total);

// 删除
int archive_delete(archive_handle_t *h, const char *id);

// 清理（按容量/数量）
int archive_cleanup(archive_handle_t *h, size_t max_size_mb, int max_count);

// 统计
int archive_get_stats(archive_handle_t *h, size_t *total_size, int *total_count);
```

## 5. 线程安全

- 所有函数线程安全，内部加锁
- 文件操作使用原子写（临时文件+rename）

## 6. 错误码

| 错误码        | 说明             |
| ------------- | ---------------- |
| OCR_ERR_IO    | 文件读写失败     |
| OCR_ERR_NOMEM | 内存不足         |
| OCR_ERR_PARAM | 路径无效         |

## 7. 示例

```c
archive_handle_t *ar = archive_create("/ocr/data/archive");
archive_item_t item;
archive_save(ar, "/tmp/cap.jpg", "Hello", "你好", &item);

archive_item_t items[10];
int total;
archive_query_t q = {.limit=10};
archive_query(ar, &q, items, 10, &total);

archive_cleanup(ar, 500, 1000);  // 限制500MB/1000条
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
