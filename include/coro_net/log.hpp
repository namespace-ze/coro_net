// =============================================================================
// coro_net/log.hpp —— 便捷伞形 include
// =============================================================================
// 大部分 .cc 只需 `#include "coro_net/log.hpp"` 即可使用 LOG_INFO/WARN/...
// =============================================================================

#pragma once

#include "coro_net/log/log_level.hpp"
#include "coro_net/log/log_stream.hpp"
#include "coro_net/log/logger.hpp"
#include "coro_net/log/async_logger.hpp"

#include <string>

namespace coro_net {

inline void init_logger(std::string basename,
                        off_t roll_size = 64 * 1024 * 1024,
                        int flush_interval_seconds = 3) {
    AsyncLogger::init(std::move(basename), roll_size, flush_interval_seconds);
}

inline void shutdown_logger() noexcept {
    AsyncLogger::shutdown();
}

}  // namespace coro_net
