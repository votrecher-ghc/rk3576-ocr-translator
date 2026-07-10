# =============================================================================
# aarch64-linux-gnu 交叉编译工具链文件
# 用法: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux.cmake ..
# =============================================================================
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 交叉编译器前缀
set(CROSS_COMPILE aarch64-linux-gnu-)

# 指定交叉编译器
set(CMAKE_C_COMPILER   ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_AR           ${CROSS_COMPILE}ar)
set(CMAKE_RANLIB       ${CROSS_COMPILE}ranlib)
set(CMAKE_STRIP        ${CROSS_COMPILE}strip)

# 目标平台 sysroot（按实际环境调整）
# set(CMAKE_SYSROOT /path/to/sysroot)
# set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})

# 仅在 sysroot 下搜索库和头文件
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 链接架构相关选项
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,/usr/lib/aarch64-linux-gnu")
