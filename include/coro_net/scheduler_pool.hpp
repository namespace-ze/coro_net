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
    // n            ：worker（Scheduler）数量
    // base         ：每个 Scheduler 的基础配置（ring_entries / sqpoll / 固定缓冲等）
    // sqpoll_threads(M)：SQPOLL 轮询线程数。仅当 base.sqpoll 为真时生效：
    //                 把 n 个 ring 分成 M 组，每组组首 ring 自建轮询线程，
    //                 组内其余 ring 经 ATTACH_WQ 复用组首的轮询线程
    //                 （M 个 kthread 服务 N 个 ring，M≤N）。M=0 等价每 ring 一个线程。
    explicit SchedulerPool(size_t n, SchedulerConfig base = {},
                           unsigned sqpoll_threads = 0);
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
