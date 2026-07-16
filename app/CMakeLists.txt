# =============================================================================
# ocr_translator 顶层 CMake 构建脚本
# 基于 RK3576 的端侧 OCR 实时翻译系统
# 单进程多线程架构，零拷贝 DMA-BUF 管线
# =============================================================================
cmake_minimum_required(VERSION 3.13)

project(ocr_translator LANGUAGES C)

# C11 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Expose the POSIX.1-2008 APIs used throughout the application (for example
# clock_gettime, sem_timedwait, O_CLOEXEC and strdup) even with strict C11.
add_definitions(-D_POSIX_C_SOURCE=200809L)

# Standard CTest switch. Cross-compilation scripts disable this explicitly;
# native builds keep it enabled so the production libraries can be exercised.
include(CTest)
option(OCR_BUILD_HARDWARE_APP
       "Build RK3576 V4L2/RGA/DRM/RKNN application targets" ON)

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

# SDK/sysroot-specific include and library paths can be supplied explicitly.
set(OCR_RGA_INCLUDE_DIR "" CACHE PATH "Directory containing im2d.h")
set(OCR_RGA_LIBRARY "" CACHE FILEPATH "Path to target librga.so")
set(OCR_RKNN_INCLUDE_DIR "" CACHE PATH "Directory containing rknn_api.h")
set(OCR_RKNN_LIBRARY "" CACHE FILEPATH "Path to librknnrt.so")
list(APPEND CMAKE_PREFIX_PATH ${CMAKE_SOURCE_DIR}/third_party)

# 查找依赖包
find_package(PkgConfig QUIET)

# libdrm
pkg_check_modules(LIBDRM QUIET libdrm)
if(NOT LIBDRM_FOUND)
    find_library(LIBDRM_LIBRARIES drm)
    find_path(LIBDRM_INCLUDE_DIRS xf86drm.h PATH_SUFFIXES libdrm)
endif()
if(NOT LIBDRM_INCLUDE_DIRS)
    find_path(LIBDRM_INCLUDE_DIRS xf86drm.h PATH_SUFFIXES libdrm)
endif()

# librga (Rockchip RGA)
pkg_check_modules(LIBRGA QUIET librga)
if(OCR_RGA_LIBRARY)
    set(LIBRGA_LIBRARIES "${OCR_RGA_LIBRARY}")
elseif(NOT LIBRGA_FOUND)
    find_library(LIBRGA_LIBRARIES rga)
endif()
if(NOT OCR_RGA_INCLUDE_DIR)
    find_path(OCR_RGA_INCLUDE_DIR im2d.h PATH_SUFFIXES rga)
endif()

# librknnrt (RKNN 运行时)
if(OCR_RKNN_LIBRARY)
    set(RKNN_LIBRARY "${OCR_RKNN_LIBRARY}")
else()
    find_library(RKNN_LIBRARY rknnrt)
endif()
if(NOT OCR_RKNN_INCLUDE_DIR)
    find_path(OCR_RKNN_INCLUDE_DIR rknn_api.h)
endif()

# libjpeg
pkg_check_modules(LIBJPEG QUIET libjpeg)
if(NOT LIBJPEG_FOUND)
    find_library(LIBJPEG_LIBRARIES jpeg)
    find_path(LIBJPEG_INCLUDE_DIRS jpeglib.h)
endif()
if(NOT LIBJPEG_INCLUDE_DIRS)
    find_path(LIBJPEG_INCLUDE_DIRS jpeglib.h)
endif()

# freetype2
pkg_check_modules(FREETYPE QUIET freetype2)
if(NOT FREETYPE_FOUND)
    find_library(FREETYPE_LIBRARIES freetype)
    find_path(FREETYPE_INCLUDE_DIRS ft2build.h PATH_SUFFIXES freetype2)
endif()

# cJSON
pkg_check_modules(CJSON QUIET libcjson)
if(NOT CJSON_FOUND)
    find_library(CJSON_LIBRARIES cjson)
    find_path(CJSON_INCLUDE_DIRS cjson/cJSON.h)
endif()
if(NOT CJSON_INCLUDE_DIRS)
    find_path(CJSON_INCLUDE_DIRS cjson/cJSON.h)
endif()

if(NOT CJSON_LIBRARIES OR NOT CJSON_INCLUDE_DIRS)
    message(FATAL_ERROR "cJSON headers/library are required")
endif()
if(OCR_BUILD_HARDWARE_APP)
    if(NOT LIBDRM_LIBRARIES OR NOT LIBDRM_INCLUDE_DIRS)
        message(FATAL_ERROR "libdrm headers/library were not found in the target sysroot")
    endif()
    if(NOT LIBRGA_LIBRARIES OR
       (NOT OCR_RGA_INCLUDE_DIR AND NOT LIBRGA_INCLUDE_DIRS))
        message(FATAL_ERROR "librga and im2d.h were not found in the target SDK/sysroot")
    endif()
    if(NOT RKNN_LIBRARY OR NOT OCR_RKNN_INCLUDE_DIR)
        message(FATAL_ERROR "librknnrt and rknn_api.h were not found in the target SDK/sysroot")
    endif()
    if(NOT LIBJPEG_LIBRARIES OR NOT LIBJPEG_INCLUDE_DIRS)
        message(FATAL_ERROR "libjpeg headers/library were not found in the target sysroot")
    endif()
    if(NOT FREETYPE_LIBRARIES OR NOT FREETYPE_INCLUDE_DIRS)
        message(FATAL_ERROR "FreeType headers/library were not found in the target sysroot")
    endif()
endif()

# 头文件包含目录
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${LIBDRM_INCLUDE_DIRS}
    ${LIBRGA_INCLUDE_DIRS}
    ${OCR_RGA_INCLUDE_DIR}
    ${LIBJPEG_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${CJSON_INCLUDE_DIRS}
    ${OCR_RKNN_INCLUDE_DIR}
)

# 子目录（按依赖顺序）
add_subdirectory(utils)
add_subdirectory(comm)
add_subdirectory(pipeline)
if(OCR_BUILD_HARDWARE_APP)
    add_subdirectory(capture)
    add_subdirectory(isp)
    add_subdirectory(display)
    add_subdirectory(ai)
    add_subdirectory(stab)
    add_subdirectory(sensors)
    add_subdirectory(archive)
    add_subdirectory(input)
    add_subdirectory(main)

    set_target_properties(ocr_translator PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    install(TARGETS ocr_translator RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../tests"
        "${CMAKE_BINARY_DIR}/tests"
    )
endif()
