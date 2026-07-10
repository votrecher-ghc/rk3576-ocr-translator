# 应用层API总览

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 模块清单](#2-模块清单)
- [3. API 设计规范](#3-api-设计规范)
- [4. 统一错误码](#4-统一错误码)
- [5. 统一数据类型](#5-统一数据类型)
- [6. 头文件组织](#6-头文件组织)

---

## 1. 概述

应用层 API 封装系统核心功能，采用 C 语言实现，以动态库形式提供。各模块 API 独立头文件，统一错误码和数据类型。

## 2. 模块清单

| 模块       | 头文件              | 库文件              | 说明               |
| ---------- | ------------------- | ------------------- | ------------------ |
| pipeline   | pipeline.h          | libpipeline.so      | 零拷贝管线         |
| capture    | capture.h           | libcapture.so       | 拍照捕获           |
| rga        | rga_wrapper.h       | librga_wrapper.so   | RGA 封装           |
| display    | display.h           | libdisplay.so       | DRM 显示           |
| ai         | ai.h                | libai.so            | OCR+翻译           |
| stab       | stab.h              | libstab.so          | 电子防抖           |
| sensors    | sensors.h           | libsensors.so       | 传感器             |
| input      | input.h             | libinput.so         | 按键输入           |
| archive    | archive.h           | libarchive.so       | 归档存储           |
| comm       | comm.h              | libcomm.so          | 通信与配置         |

## 3. API 设计规范

- 命名：`<module>_<action>`，如 `pipeline_init()`
- 返回值：`int`，0 成功，负数错误码
- 句柄：`typedef struct xxx_ctx xxx_handle_t;`
- 线程安全：每个文档标注
- 资源管理：成对的 init/destroy、acquire/release

## 4. 统一错误码

```c
typedef enum {
    OCR_OK = 0,           // 成功
    OCR_ERR_PARAM = -1,   // 参数错误
    OCR_ERR_NOMEM = -2,   // 内存不足
    OCR_ERR_NODEV = -3,   // 设备不存在
    OCR_ERR_BUSY  = -4,   // 设备忙
    OCR_ERR_TIMEOUT=-5,   // 超时
    OCR_ERR_IOCTL  = -6,  // ioctl 失败
    OCR_ERR_NPU    = -7,  // NPU 推理失败
    OCR_ERR_IO     = -8,  // 文件 I/O 失败
    OCR_ERR_UNSUPP = -9,  // 不支持
    OCR_ERR_INTERNAL=-10, // 内部错误
} ocr_err_t;
```

## 5. 统一数据类型

```c
// DMA-BUF 缓冲
typedef struct {
    int fd;
    void *mapped;
    size_t size;
    int width, height;
    uint32_t format;
} ocr_dmabuf_t;

// OCR 结果
typedef struct {
    char text[1024];
    int box[8];  // 4点坐标
} ocr_text_t;

// 翻译结果
typedef struct {
    char src[1024];
    char dst[1024];
} ocr_translate_t;
```

## 6. 头文件组织

```
ocr/include/
├── ocr_common.h       // 公共类型与错误码
├── pipeline.h
├── capture.h
├── rga_wrapper.h
├── display.h
├── ai.h
├── stab.h
├── sensors.h
├── input.h
├── archive.h
└── comm.h
```

> 各模块详细 API 见后续文档。

---

> 相关文档：[02-pipeline模块API.md](02-pipeline模块API.md) 及后续各模块文档
