// =============================================================================
// coro_net/idle_connection_wheel.hpp —— 每个 worker 持有的空闲连接时间轮
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/idle_entry.hpp"
#include "../../src/util/circular_buffer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_set>

namespace coro_net {

class TcpConnection;

class IdleConnectionWheel {
public:
    using Bucket = std::unordered_set<std::shared_ptr<IdleEntry>>;

    IdleConnectionWheel(Scheduler& s, std::chrono::seconds idle);

    // 启动 1Hz 滴答协程
    void start();
    void stop() { running_.store(false, std::memory_order_relaxed); }

    // 给新连接发一个 entry：返回值给 TcpConnection 持 weak_ptr
    std::shared_ptr<IdleEntry> register_conn(std::weak_ptr<TcpConnection> c);

    // 续命：把 entry 重新插入队尾桶
    void refresh(const std::shared_ptr<IdleEntry>& e);

private:
    Task<void> tick_coro();

    Scheduler* sched_;
    util::CircularBuffer<Bucket> buckets_;
    std::atomic_bool running_{false};
};

}  // namespace coro_net
