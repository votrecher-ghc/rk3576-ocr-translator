# RKNN-API参考笔记

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 核心 API](#2-核心-api)
- [3. 数据结构](#3-数据结构)
- [4. 使用流程](#4-使用流程)
- [5. 注意事项](#5-注意事项)

---

## 1. 概述

RKNN API 是 RKNPU2 用户态接口，用于加载 RKNN 模型并执行 NPU 推理。

## 2. 核心 API

```c
// 初始化
int rknn_init(rknn_context *ctx, void *model, uint32_t size, uint32_t flag);

// 输入设置
int rknn_query(rknn_context ctx, rknn_query_cmd cmd, void *info, uint32_t size);
int rknn_inputs_set(rknn_context ctx, uint32_t n_inputs, rknn_input *inputs);

// 推理
int rknn_run(rknn_context ctx, rknn_run_extend *extend);

// 输出获取
int rknn_outputs_get(rknn_context ctx, uint32_t n_outputs, rknn_output *outputs, rknn_output_extend *extends);
int rknn_outputs_release(rknn_context ctx, uint32_t n_outputs, rknn_output *outputs);

// 销毁
int rknn_destroy(rknn_context *ctx);
```

## 3. 数据结构

```c
typedef struct {
    int fd;            // DMA-BUF fd（零拷贝）
    void *buf;         // 或内存指针
    uint32_t size;
    int type;          // RKNN_TENSOR_UINT8
    int fmt;           // RKNN_TENSOR_NHWC
} rknn_input;

typedef struct {
    int want_float;    // 是否输出 float
    uint32_t index;
    void *buf;
    uint32_t size;
} rknn_output;
```

## 4. 使用流程

```
1. rknn_init(加载模型)
2. rknn_query(查询输入/输出属性)
3. 循环:
   a. rknn_inputs_set(设置输入，可用 DMA-BUF fd)
   b. rknn_run(推理)
   c. rknn_outputs_get(获取输出)
   d. 处理输出
   e. rknn_outputs_release
4. rknn_destroy
```

## 5. 注意事项

- 使用 DMA-BUF fd 实现零拷贝输入
- 模型需 INT8 量化以获得最佳性能
- NPU 串行执行，多线程推理需排队
- 推理耗时与模型大小/输入尺寸相关

---

> 相关文档：[05-编译构建/06-RKNN模型转换指南.md](../05-编译构建/06-RKNN模型转换指南.md)
