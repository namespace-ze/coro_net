# =============================================================================
# coro_net 库的 CMake 构建文件
# =============================================================================
# 这个库是 C++20 协程 + io_uring 异步网络库，独立于 mymuduo。
# 依赖：
#   - 编译器：g++ 13+ / clang 17+（需要 C++20 协程完整支持）
#   - liburing：sudo apt install liburing-dev
# 输出：libcoro_net.so 放到顶层 lib/ 目录
# =============================================================================

cmake_minimum_required(VERSION 3.13)

project(coro_net LANGUAGES CXX)

# 这是一个独立子项目，强制 C++20（顶层项目目前还是 C++17，
# 等到 S8 阶段集成时再把顶层切到 20。这里先用 target 级别的设置）。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 调试期保留符号；正式 benchmark 时可手动改成 Release/-O2
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# -----------------------------------------------------------------------------
# 查找 liburing
# -----------------------------------------------------------------------------
# liburing 是 Linux 官方提供的 io_uring 用户态封装库（作者 Jens Axboe）。
# 不直接用 syscall 而是用它的原因：
#   1) 帮你处理 SQ/CQ 环形缓冲区的 mmap 与下标推进
#   2) 提供 io_uring_prep_* 一组便捷函数填 SQE
#   3) 跨内核版本兼容性兜底
find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED liburing)

# -----------------------------------------------------------------------------
# 收集源文件
# -----------------------------------------------------------------------------
file(GLOB_RECURSE CORO_NET_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc
)

# -----------------------------------------------------------------------------
# 构建共享库
# -----------------------------------------------------------------------------
add_library(coro_net SHARED ${CORO_NET_SOURCES})

target_include_directories(coro_net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${URING_INCLUDE_DIRS}
)

target_link_libraries(coro_net
    PUBLIC
        ${URING_LIBRARIES}
        pthread
)

target_compile_options(coro_net PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fcoroutines  # gcc 显式开启协程支持（gcc 13 默认已开，加上保险）
)

# -----------------------------------------------------------------------------
# 子项目（example / test 在 S3 之后才有实际内容）
# -----------------------------------------------------------------------------
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test/CMakeLists.txt)
    enable_testing()
    add_subdirectory(test)
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/example/CMakeLists.txt)
    add_subdirectory(example)
endif()
