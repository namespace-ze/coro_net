// =============================================================================
// coro_net/log/log_level.hpp —— 日志级别定义
// =============================================================================
// 单独成头，让其它模块（Logger / async backend）共享枚举且不互相循环 include。
// =============================================================================

#pragma once

#include <cstdint>

namespace coro_net {

enum class LogLevel : int8_t {
    TRACE = 0,
    DEBUG = 1,
    INFO  = 2,
    WARN  = 3,
    ERROR = 4,
    FATAL = 5,
    NUM_LOG_LEVELS = 6,
};

// 返回 6 字符等宽字符串（"TRACE ", "DEBUG ", "INFO  ", "WARN  ", "ERROR ", "FATAL "）
// 等宽便于日志列对齐。
const char* log_level_name(LogLevel lv) noexcept;

}  // namespace coro_net

// -----------------------------------------------------------------------------
// 编译期级别下限：低于此级别的 LOG_* 在预处理阶段直接被剪除。
// 默认 INFO；要看 DEBUG/TRACE 时编译加 -DCORO_NET_LOG_MIN_LEVEL_VAL=1（DEBUG=1，TRACE=0）。
// -----------------------------------------------------------------------------
#ifndef CORO_NET_LOG_MIN_LEVEL_VAL
#define CORO_NET_LOG_MIN_LEVEL_VAL 2   // INFO
#endif
