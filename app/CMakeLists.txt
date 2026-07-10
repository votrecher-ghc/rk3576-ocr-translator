# =============================================================================
# ocr_translator 顶层 CMake 构建脚本
# 基于 RK3576 的端侧 OCR 实时翻译系统
# 单进程多线程架构，零拷贝 DMA-BUF 管线
# =============================================================================
cmake_minimum_required(VERSION 3.10)

project(ocr_translator LANGUAGES C)

# C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# 构建类型
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 编译选项
add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -ffunction-sections
    -fdata-sections
    -fomit-frame-pointer
)

# Release 优化
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
endif()

# 链接时删除未使用段
add_link_options(-Wl,--gc-sections)

# 依赖库查找路径（可根据目标设备 sysroot 调整）
list(APPEND CMAKE_PREFIX_PATH
    ${CMAKE_SOURCE_DIR}/third_party
    /usr/lib/aarch64-linux-gnu
)

# 查找依赖包
find_package(PkgConfig QUIET)

# libdrm
pkg_check_modules(LIBDRM QUIET libdrm)
if(NOT LIBDRM_FOUND)
    set(LIBDRM_LIBRARIES drm)
    set(LIBDRM_INCLUDE_DIRS /usr/include/libdrm)
endif()

# librga (Rockchip RGA)
pkg_check_modules(LIBRGA QUIET librga)
if(NOT LIBRGA_FOUND)
    set(LIBRGA_LIBRARIES rga)
endif()

# librknnrt (RKNN 运行时)
find_library(RKNN_LIBRARY rknnrt PATHS /usr/lib/aarch64-linux-gnu)
if(NOT RKNN_LIBRARY)
    set(RKNN_LIBRARY rknnrt)
endif()

# libjpeg
pkg_check_modules(LIBJPEG QUIET libjpeg)
if(NOT LIBJPEG_FOUND)
    set(LIBJPEG_LIBRARIES jpeg)
endif()

# freetype2
pkg_check_modules(FREETYPE QUIET freetype2)
if(NOT FREETYPE_FOUND)
    set(FREETYPE_LIBRARIES freetype)
endif()

# cJSON
pkg_check_modules(CJSON QUIET libcjson)
if(NOT CJSON_FOUND)
    set(CJSON_LIBRARIES cjson)
endif()

# 头文件包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${LIBDRM_INCLUDE_DIRS}
    ${LIBRGA_INCLUDE_DIRS}
    ${LIBJPEG_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
)

# 子目录（按依赖顺序）
add_subdirectory(utils)
add_subdirectory(comm)
add_subdirectory(pipeline)
add_subdirectory(capture)
add_subdirectory(isp)
add_subdirectory(display)
add_subdirectory(ai)
add_subdirectory(stab)
add_subdirectory(sensors)
add_subdirectory(input)
add_subdirectory(archive)
add_subdirectory(main)

# 可执行文件输出路径
set_target_properties(ocr_translator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# 安装规则
install(TARGETS ocr_translator RUNTIME DESTINATION bin)
