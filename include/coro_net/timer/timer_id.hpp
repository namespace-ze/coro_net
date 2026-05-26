// =============================================================================
// coro_net/timer/timer_id.hpp —— TimerId 句柄
// =============================================================================
// 用于在 cancel 时定位 timer。包含一个单调递增的 sequence，避免 ABA。
// =============================================================================

#pragma once

#include <cstdint>

namespace coro_net {

class Timer;
using TimerSequence = uint64_t;

struct TimerId {
    Timer*        timer_ = nullptr;
    TimerSequence seq_   = 0;

    bool valid() const noexcept { return timer_ != nullptr; }
};

}  // namespace coro_net
