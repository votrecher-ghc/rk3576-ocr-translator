# 应用CMake构建指南

| 版本 | 日期       | 作者   | 变更说明 |
| ---- | ---------- | ------ | -------- |
| v1.0 | 2026-07-10 | 项目组 | 初始版本 |

---

## 目录

- [1. 概述](#1-概述)
- [2. 目录结构](#2-目录结构)
- [3. CMakeLists.txt](#3-cmakeliststxt)
- [4. 交叉编译配置](#4-交叉编译配置)
- [5. 编译](#5-编译)

---

## 1. 概述

应用层使用 CMake 构建，交叉编译为 aarch64 目标。

## 2. 目录结构

```
ocr/
├── CMakeLists.txt
├── cmake/
│   └── aarch64-linux-gnu.cmake
├── include/
├── src/
│   ├── pipeline/
│   ├── ai/
│   └── ...
└── third_party/
    ├── libdrm/
    ├── librga/
    └── rknn_api/
```

## 3. CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(ocr_translate C)

set(CMAKE_C_STANDARD 11)

# 交叉编译
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/aarch64-linux-gnu.cmake)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party/libdrm/include
    ${CMAKE_SOURCE_DIR}/third_party/librga/include
    ${CMAKE_SOURCE_DIR}/third_party/rknn_api/include
)

link_directories(
    ${CMAKE_SOURCE_DIR}/third_party/libdrm/lib
    ${CMAKE_SOURCE_DIR}/third_party/librga/lib
    ${CMAKE_SOURCE_DIR}/third_party/rknn_api/lib
)

add_executable(ocr_translate src/main.c src/pipeline/pipeline.c ...)
target_link_libraries(ocr_translate drm rga rknnrt pthread json-c)
```

## 4. 交叉编译配置

```cmake
# cmake/aarch64-linux-gnu.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
```

## 5. 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
# 产物：ocr_translate
```

---

> 相关文档：[08-一键构建脚本说明.md](08-一键构建脚本说明.md)
