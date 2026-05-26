// =============================================================================
// coro_net/timer/timer_queue.hpp —— per-Scheduler timerfd + 排序堆
// =============================================================================

#pragma once

#include "coro_net/timer/timer.hpp"
#include "coro_net/timer/timer_id.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

namespace coro_net {

class Scheduler;
class IoOperationBase;

class TimerQueue {
public:
    explicit TimerQueue(Scheduler& sched);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // 由 Scheduler::run() 调用：把 TimerfdWatcher 挂上 ring
    void start();

    // 同一线程调用：插入一次性 / 重复 timer
    TimerId add(std::function<void()> cb, std::chrono::nanoseconds delay);
    TimerId add_periodic(std::function<void()> cb, std::chrono::nanoseconds interval);

    // 取消：O(log n)，幂等
    void cancel(TimerId id);

    // CQE 回来时被 TimerfdWatcher 调用
    void handle_expired();

private:
    using TimePoint = Timer::TimePoint;
    using Entry     = std::pair<TimePoint, Timer*>;

    void reset_timerfd(TimePoint when);
    void disarm_timerfd();

    class TimerfdWatcher;

    Scheduler*    sched_;
    int           timerfd_ = -1;
    std::unique_ptr<TimerfdWatcher> watcher_;

    std::atomic<TimerSequence> next_seq_{1};
    std::set<Entry>             timers_;
    std::unordered_map<TimerSequence, Timer*> active_;

    // 处于 handle_expired 调用过程中的标记
    bool                         calling_expired_ = false;
    std::vector<TimerSequence>   canceling_seqs_;
};

}  // namespace coro_net
