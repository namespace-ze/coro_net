// =============================================================================
// scheduler_pool.cc — SchedulerPool 实现
// =============================================================================
// SchedulerPool 是 Scheduler 的多线程容器：每个线程绑定一个 Scheduler，
// 通过 round-robin 把新工作派发到不同 worker 上。
// =============================================================================
#include "coro_net/scheduler_pool.hpp"
#include "coro_net/io/io_uring.h"
#include "coro_net/log.hpp"

namespace coro_net {

SchedulerPool::SchedulerPool(size_t n, SchedulerConfig base, unsigned M,
                             std::vector<int> sqpoll_cpus) {
    schedulers_.reserve(n);

    // 是否做 SQPOLL 分组：把 n 个 ring 分成 M 组，组首自建轮询线程，
    // 其余经 ATTACH_WQ 复用组首线程。M=0（或非 sqpoll）时退化为每 ring 独立。
    const bool   grouping = base.sqpoll && M > 0;
    const size_t gsz = grouping ? (n + M - 1) / M : 1;  // ceil(n/M)

    for (size_t i = 0; i < n; ++i) {
        SchedulerConfig cfg = base;
        cfg.wq_fd = -1;
        cfg.sq_thread_cpu = -1;
        if (grouping) {
            const size_t g = i / gsz;                    // 本 ring 所属组
            const size_t leader = g * gsz;               // 组首下标
            if (i == leader) {
                // 组首：自建轮询线程，可钉核（sqpoll_cpus[g]）。
                if (g < sqpoll_cpus.size()) cfg.sq_thread_cpu = sqpoll_cpus[g];
            } else {
                // 组员：复用组首的轮询线程（不再单独钉核）。
                cfg.wq_fd = schedulers_[leader]->ring().ring_fd();
            }
        }
        schedulers_.emplace_back(std::make_unique<Scheduler>(cfg));
    }

    if (grouping) {
        LOG_INFO << "SchedulerPool: " << n << " rings, " << M
                 << " SQPOLL poller thread(s) (group size " << gsz << ")"
                 << (sqpoll_cpus.empty() ? " [pollers unpinned]"
                                         : " [pollers pinned]");
    }
}

SchedulerPool::~SchedulerPool() {
    if (started_) {
        stop_all();
        wait();
    }
}

void SchedulerPool::start() {
    started_ = true;
    threads_.reserve(schedulers_.size());
    for (auto& sch : schedulers_) {
        Scheduler* p = sch.get();
        threads_.emplace_back([p] { p->run(); });
    }
}

void SchedulerPool::stop_all() {
    // 注意：本函数通常在主线程（没有 ring 的线程）调用。
    // Scheduler::stop 在跨线程 + 无 ring 情况下不能唤醒目标。
    // 解决：我们让每个 worker 自己定期检查 stopping_ —— 不行，那需要 timeout。
    // 简化做法：要求 stop_all 在某个 scheduler 线程上调用（如把它包成
    // 一个发给 worker[0] 的 task）。
    //
    // 但更常见的用法是主线程 join 等结束，我们这里做一个权宜：
    // 在每个 scheduler 自己 ring 上 *用一个 MSG_RING SQE 从我们这条线程* —
    // 但我们没 ring 啊...
    //
    // 解决方案：每个 worker 在自己线程内监听一个"stop 信号"。最简单的实现：
    // 在每个 scheduler 上 spawn 一个监听 stop 的协程。但这又要协程。
    //
    // 为 S4 简化：要求调用方先把所有 worker 都引导到一个 "drain → stop" 协程
    // （由用户代码控制何时停）。stop_all() 直接设标志，依赖
    // worker 自己 spawn 的协程会在适当时机调用本 scheduler 的 stop()。
    //
    // 这里只设置每个 scheduler 的 stopping_ 标志；MSG_RING 唤醒在 S6/兜底里做。
    for (auto& sch : schedulers_) {
        sch->stop();   // 同进程内 stop()，跨线程时只设标志，依赖现有 CQE 唤醒
    }
}

void SchedulerPool::wait() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    started_ = false;
}

Scheduler& SchedulerPool::next() noexcept {
    size_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed) %
                 schedulers_.size();
    return *schedulers_[idx];
}

}  // namespace coro_net
