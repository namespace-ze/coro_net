// =============================================================================
// timer_queue.cc —— TimerQueue + TimerfdWatcher 实现
// =============================================================================
#include "coro_net/timer/timer_queue.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io_operation.hpp"
#include "coro_net/io/io_uring.h"
#include "coro_net/log.hpp"

#include <liburing.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace coro_net {

// -----------------------------------------------------------------------------
// TimerfdWatcher —— 与 EventfdWatcher 同构：永久挂一个 read，CQE 后 re-arm
// -----------------------------------------------------------------------------
class TimerQueue::TimerfdWatcher : public IoOperationBase {
public:
    TimerfdWatcher(Scheduler& s, TimerQueue* q) : IoOperationBase(s), queue_(q) {}

    void rearm(io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, queue_->timerfd_, &dummy_, sizeof(dummy_), 0);
        io_uring_sqe_set_data(sqe, this);
    }

    void on_complete(int32_t res, uint32_t /*flags*/) noexcept override {
        if (res >= 0) {
            queue_->handle_expired();
        } else if (res != -ECANCELED) {
            LOG_ERROR << "timerfd read failed res=" << res;
        }
        // 总是 re-arm，避免某次错误后整个 timer 通道挂掉
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        if (sqe) rearm(sqe);
    }

private:
    TimerQueue* queue_;
    uint64_t    dummy_ = 0;
};

// -----------------------------------------------------------------------------
// TimerQueue 构造 / 析构
// -----------------------------------------------------------------------------
TimerQueue::TimerQueue(Scheduler& sched) : sched_(&sched) {
    timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd_ < 0) {
        throw std::system_error(errno, std::system_category(), "timerfd_create");
    }
    watcher_ = std::make_unique<TimerfdWatcher>(*sched_, this);
}

TimerQueue::~TimerQueue() {
    if (timerfd_ >= 0) ::close(timerfd_);
    for (auto& e : timers_) delete e.second;
}

void TimerQueue::start() {
    io_uring_sqe* sqe = sched_->ring().get_sqe();
    if (sqe) watcher_->rearm(sqe);
}

// -----------------------------------------------------------------------------
// reset_timerfd / disarm_timerfd
// -----------------------------------------------------------------------------
void TimerQueue::reset_timerfd(TimePoint when) {
    auto now = std::chrono::steady_clock::now();
    auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(when - now);

    itimerspec spec{};
    // 防御：内核把 {0,0} 视为 disarm；clamp 到 1ns
    long ns = static_cast<long>(delta.count());
    if (ns <= 0) ns = 1;
    spec.it_value.tv_sec  = ns / 1'000'000'000;
    spec.it_value.tv_nsec = ns % 1'000'000'000;
    // it_interval 不用：重复 timer 在 handle_expired 里手动 restart
    if (::timerfd_settime(timerfd_, 0, &spec, nullptr) < 0) {
        LOG_ERROR << "timerfd_settime failed errno=" << errno;
    }
}

void TimerQueue::disarm_timerfd() {
    itimerspec spec{};   // 全 0：disarm
    ::timerfd_settime(timerfd_, 0, &spec, nullptr);
}

// -----------------------------------------------------------------------------
// add / add_periodic
// -----------------------------------------------------------------------------
TimerId TimerQueue::add(std::function<void()> cb, std::chrono::nanoseconds delay) {
    auto when = std::chrono::steady_clock::now() + delay;
    auto seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
    Timer* t = new Timer(std::move(cb), when,
                         std::chrono::nanoseconds(0), seq);

    bool was_empty_or_new_top = timers_.empty() || when < timers_.begin()->first;
    timers_.insert({when, t});
    active_[seq] = t;
    if (was_empty_or_new_top) reset_timerfd(when);

    LOG_DEBUG << "TimerQueue::add seq=" << seq
              << " delay_ns=" << delay.count();
    return TimerId{t, seq};
}

TimerId TimerQueue::add_periodic(std::function<void()> cb,
                                 std::chrono::nanoseconds interval) {
    auto when = std::chrono::steady_clock::now() + interval;
    auto seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
    Timer* t = new Timer(std::move(cb), when, interval, seq);

    bool was_empty_or_new_top = timers_.empty() || when < timers_.begin()->first;
    timers_.insert({when, t});
    active_[seq] = t;
    if (was_empty_or_new_top) reset_timerfd(when);

    LOG_DEBUG << "TimerQueue::add_periodic seq=" << seq
              << " interval_ns=" << interval.count();
    return TimerId{t, seq};
}

// -----------------------------------------------------------------------------
// cancel —— 通过 sequence 查找；幂等
// -----------------------------------------------------------------------------
void TimerQueue::cancel(TimerId id) {
    if (!id.valid()) return;
    auto it = active_.find(id.seq_);
    if (it == active_.end()) {
        // 已经触发或被取消；如果正在 expired 列表里，记下序列号让 handle_expired 跳过重插
        if (calling_expired_) canceling_seqs_.push_back(id.seq_);
        return;
    }
    Timer* t = it->second;
    bool was_top = !timers_.empty() && timers_.begin()->second == t;
    timers_.erase({t->expiration(), t});
    active_.erase(it);

    if (calling_expired_) {
        // 此 timer 此刻不在 timers_/active_ 里（已被弹出），但 expired 列表中可能还在
        canceling_seqs_.push_back(id.seq_);
    }
    delete t;

    LOG_DEBUG << "TimerQueue::cancel seq=" << id.seq_;
    if (was_top) {
        if (timers_.empty()) disarm_timerfd();
        else reset_timerfd(timers_.begin()->first);
    }
}

// -----------------------------------------------------------------------------
// handle_expired —— TimerfdWatcher CQE 来时调用
// -----------------------------------------------------------------------------
void TimerQueue::handle_expired() {
    auto now = std::chrono::steady_clock::now();

    std::vector<Timer*> expired;
    while (!timers_.empty()) {
        auto it = timers_.begin();
        if (it->first > now) break;
        expired.push_back(it->second);
        active_.erase(it->second->sequence());
        timers_.erase(it);
    }

    calling_expired_ = true;
    for (Timer* t : expired) {
        try {
            t->run();
        } catch (const std::exception& e) {
            LOG_ERROR << "timer callback threw: " << e.what();
        } catch (...) {
            LOG_ERROR << "timer callback threw (unknown)";
        }
    }
    calling_expired_ = false;

    // 重复 timer：未被 cancel 的重新插入
    for (Timer* t : expired) {
        bool was_canceled = false;
        for (auto seq : canceling_seqs_) {
            if (seq == t->sequence()) { was_canceled = true; break; }
        }
        if (t->repeat() && !was_canceled) {
            t->restart(now);
            timers_.insert({t->expiration(), t});
            active_[t->sequence()] = t;
        } else {
            delete t;
        }
    }
    canceling_seqs_.clear();

    if (timers_.empty()) disarm_timerfd();
    else reset_timerfd(timers_.begin()->first);
}

}  // namespace coro_net
