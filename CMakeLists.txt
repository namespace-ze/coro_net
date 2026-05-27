# =============================================================================
# coro_net 库的 CMake 构建文件
# =============================================================================
# 这个库是 C++20 协程 + io_uring 异步网络库。
# 依赖：
#   - 编译器：g++ 13+ 或 clang 14+
#   - liburing：sudo apt install liburing-dev
#   - 用 clang 时还需 libc++（避开 clang 14 + libstdc++ 13 的 <chrono> consteval bug）：
#       sudo apt install libc++-14-dev libc++abi-14-dev
# 输出：libcoro_net.so 放到顶层 lib/ 目录
# =============================================================================

cmake_minimum_required(VERSION 3.13)

project(coro_net LANGUAGES CXX)

#让 CMake 在构建时生成一个 compile_commands.json 文件
#这个文件会精确记录每一个 .cc 文件是用什么参数、什么路径编译的
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
)

# -----------------------------------------------------------------------------
# 编译器特定 flag
# -----------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # -fcoroutines 是 GCC 专属（gcc 13 默认已开，加上保险）
    target_compile_options(coro_net PRIVATE -fcoroutines)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang 14 + libstdc++ 13 的 <chrono> 有 consteval bug，
    # 切到 libc++（需先 apt install libc++-XX-dev libc++abi-XX-dev）。
    # PUBLIC 传递给依赖此库的 example / test，保持 ABI 一致。
    target_compile_options(coro_net PUBLIC -stdlib=libc++)
    target_link_options(coro_net PUBLIC -stdlib=libc++)
    target_link_libraries(coro_net PUBLIC c++abi)
endif()

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
