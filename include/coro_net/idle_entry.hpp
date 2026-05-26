// =============================================================================
// coro_net/idle_entry.hpp —— 时间轮挂载条目
// =============================================================================
//
// 【生命周期】
//   - shared_ptr<IdleEntry> 仅由 IdleConnectionWheel 的桶持有
//   - TcpConnection 内只保留 weak_ptr<IdleEntry>，避免循环
//   - 每次 recv 后，TcpConnection 用 weak.lock() 拿到 entry，
//     IdleConnectionWheel 把 entry 重新插入"队尾桶" → 续命
//   - 满 idle_seconds 没续命 → entry 在所有桶中的 shared 计数降到 0
//     → ~IdleEntry 触发 → 在 sched 上 spawn 一个关闭协程关闭连接
// =============================================================================

#pragma once

#include <memory>

namespace coro_net {

class Scheduler;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

struct IdleEntry {
    std::weak_ptr<TcpConnection> wconn;
    Scheduler* sched = nullptr;

    // 显式构造函数：避免聚合初始化 IdleEntry{c, sched} 产生的临时对象
    // 在 make_shared 内部 *move 构造* 给堆对象前 / 后被析构，从而误触发
    // 关闭协程。直接 make_shared<IdleEntry>(c, sched) 不产生 IdleEntry 临时。
    IdleEntry(std::weak_ptr<TcpConnection> c, Scheduler* s) noexcept
        : wconn(std::move(c)), sched(s) {}

    ~IdleEntry();
};

}  // namespace coro_net
