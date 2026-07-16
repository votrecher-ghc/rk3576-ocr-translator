# =============================================================================
# aarch64-linux-gnu 交叉编译工具链文件
# 用法: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake ..
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译器前缀。可通过环境变量 CROSS_COMPILE 覆盖，例如
# aarch64-buildroot-linux-gnu-。
set(CROSS_COMPILE "$ENV{CROSS_COMPILE}" CACHE STRING "Cross compiler prefix")
if(CROSS_COMPILE STREQUAL "")
    set(CROSS_COMPILE "aarch64-none-linux-gnu-" CACHE STRING
        "Cross compiler prefix" FORCE)
endif()

# 指定交叉编译器
set(CMAKE_C_COMPILER   "${CROSS_COMPILE}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_COMPILE}g++")
set(CMAKE_AR           "${CROSS_COMPILE}ar")
set(CMAKE_RANLIB       "${CROSS_COMPILE}ranlib")
set(CMAKE_STRIP        "${CROSS_COMPILE}strip")

# 目标平台 sysroot 必须由 build_app.sh 验证后传入。没有 sysroot 时，
# host pkg-config/find_library 可能静默接受宿主机头文件和共享库。
if(NOT DEFINED ENV{SYSROOT} OR "$ENV{SYSROOT}" STREQUAL "")
    message(FATAL_ERROR
        "SYSROOT is required; point it at the vendor SDK target sysroot")
endif()
file(TO_CMAKE_PATH "$ENV{SYSROOT}" OCR_SYSROOT_INPUT)
get_filename_component(OCR_SYSROOT "${OCR_SYSROOT_INPUT}" REALPATH)
if(NOT IS_DIRECTORY "${OCR_SYSROOT}" OR OCR_SYSROOT STREQUAL "/")
    message(FATAL_ERROR "Invalid or unsafe target SYSROOT: ${OCR_SYSROOT}")
endif()
set(CMAKE_SYSROOT "${OCR_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${OCR_SYSROOT}")

# Always replace pkg-config search variables. Inheriting PKG_CONFIG_PATH or a
# host PKG_CONFIG_LIBDIR would bypass CMake's root-path modes below.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${OCR_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${OCR_SYSROOT}/usr/lib/pkgconfig:${OCR_SYSROOT}/usr/share/pkgconfig:${OCR_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${OCR_SYSROOT}/lib/pkgconfig:${OCR_SYSROOT}/lib/aarch64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH FALSE CACHE BOOL
    "Do not add host CMake prefixes to target pkg-config searches" FORCE)

# 仅在 sysroot 下搜索库和头文件
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CMake 会从 sysroot 推导链接搜索路径；不要混入宿主机 /usr/lib。
