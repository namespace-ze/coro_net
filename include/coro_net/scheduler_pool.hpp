// =============================================================================
// coro_net/scheduler_pool.hpp —— 多线程 Scheduler 容器
// =============================================================================
//
// 【用途】TcpServer 启动时构造一个 SchedulerPool，每个 worker 线程跑一个
//        Scheduler::run()。新连接到来时 round-robin 选下一个 worker。
//
// 【接口】
//   - start()：拉起 N 个线程，每个跑一个 Scheduler::run()
//   - stop_all()：让所有 Scheduler 退出循环
//   - wait()：join 所有线程
//   - next()：round-robin 选下一个 Scheduler 引用
//   - at(i)：拿到第 i 个 Scheduler 引用
//
// 【生命周期】
//   构造时 *仅* 创建 Scheduler 对象（在主线程的栈上构造，未运行）；
//   start() 拉起线程；析构前必须 stop_all + wait。
// =============================================================================

#pragma once

#include "coro_net/scheduler.hpp"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace coro_net {

class SchedulerPool {
public:
    explicit SchedulerPool(size_t n);
    ~SchedulerPool();

    SchedulerPool(const SchedulerPool&) = delete;
    SchedulerPool& operator=(const SchedulerPool&) = delete;

    void start();
    void stop_all();
    void wait();

    size_t size() const noexcept { return schedulers_.size(); }
    Scheduler& at(size_t i) noexcept { return *schedulers_[i]; }
    Scheduler& next() noexcept;

private:
    std::vector<std::unique_ptr<Scheduler>> schedulers_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_idx_{0};
    bool started_ = false;
};

}  // namespace coro_net
