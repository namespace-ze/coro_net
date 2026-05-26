// =============================================================================
// coro_net/timer/timer.hpp —— TimerQueue 的节点对象
// =============================================================================

#pragma once

#include "coro_net/timer/timer_id.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace coro_net {

class Timer {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration  = std::chrono::nanoseconds;

    Timer(std::function<void()> cb, TimePoint when,
          Duration interval, TimerSequence seq) noexcept
        : callback_(std::move(cb)),
          expiration_(when),
          interval_(interval),
          repeat_(interval.count() > 0),
          sequence_(seq) {}

    void run() const { callback_(); }

    TimePoint     expiration() const noexcept { return expiration_; }
    Duration      interval()   const noexcept { return interval_; }
    bool          repeat()     const noexcept { return repeat_; }
    TimerSequence sequence()   const noexcept { return sequence_; }

    // 重复 timer 触发后：next_expiration = now + interval（fixed-delay）
    void restart(TimePoint now) noexcept {
        expiration_ = now + interval_;
    }

private:
    std::function<void()> callback_;
    TimePoint     expiration_;
    Duration      interval_;
    bool          repeat_;
    TimerSequence sequence_;
};

}  // namespace coro_net
