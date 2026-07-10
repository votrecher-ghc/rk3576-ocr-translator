# ai模块API

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

ai 模块封装 RKNN OCR 推理与离线翻译引擎，接受 DMA-BUF 输入，输出 OCR 文本与翻译结果。

## 2. 文件列表

| 文件          | 说明               |
| ------------- | ------------------ |
| ai.h          | API 声明           |
| ocr.c         | OCR 推理           |
| translate.c   | 翻译引擎           |

## 3. 数据结构

```c
typedef struct ai_ctx ai_handle_t;

typedef struct {
    char text[1024];
    int box[8];      // 4点坐标
    float score;
} ai_ocr_item_t;

typedef struct {
    ai_ocr_item_t items[32];
    int count;
} ai_ocr_result_t;

typedef struct {
    char src[2048];
    char dst[2048];
} ai_translate_result_t;
```

## 4. 函数API

```c
ai_handle_t *ai_create(const char *det_model, const char *rec_model, const char *dict);
void ai_destroy(ai_handle_t *h);

// OCR（输入DMA-BUF fd）
int ai_ocr_infer(ai_handle_t *h, int dmabuf_fd, int w, int h, ai_ocr_result_t *result);

// 翻译
int ai_translate(ai_handle_t *h, const char *src, ai_translate_result_t *result);

// 设置语言对
int ai_set_lang(ai_handle_t *h, const char *src_lang, const char *dst_lang);
```

## 5. 线程安全

- `create/destroy`：非线程安全
- `ocr_infer`：线程安全（NPU 串行执行）
- `translate`：线程安全
- `set_lang`：非线程安全，需停止推理后调用

## 6. 错误码

| 错误码        | 说明             |
| ------------- | ---------------- |
| OCR_ERR_NPU   | NPU 推理失败     |
| OCR_ERR_PARAM | 模型路径无效     |
| OCR_ERR_NOMEM | 模型加载内存不足 |

## 7. 示例

```c
ai_handle_t *ai = ai_create("ocr_det.rknn", "ocr_rec.rknn", "dict.bin");
ai_ocr_result_t ocr;
ai_ocr_infer(ai, dmabuf_fd, 320, 320, &ocr);

ai_translate_result_t tr;
ai_translate(ai, ocr.items[0].text, &tr);
```

---

> 相关文档：[01-应用层API总览.md](01-应用层API总览.md)
