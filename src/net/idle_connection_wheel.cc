// =============================================================================
// idle_connection_wheel.cc — IdleConnectionWheel 实现
// =============================================================================
// 时间轮淘汰超时空闲连接。IdleEntry::~IdleEntry 与轮盘语义紧耦合（析构
// 即触发关闭），所以也放在本文件。
// =============================================================================
#include "coro_net/tcp.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/log.hpp"

namespace coro_net {

// =============================================================================
// IdleEntry 析构 —— 时间轮把这个 entry 完全淘汰时（所有桶都没有引用），
// 触发关闭对应连接的协程
// =============================================================================
IdleEntry::~IdleEntry() {
    auto c = wconn.lock();
    if (!c || !sched) return;
    LOG_DEBUG << "IdleEntry evicting fd=" << c->fd();

    // spawn 关闭协程：必须在 sched 所属的 worker 线程上运行；
    // 但 ~IdleEntry 是从 buckets_.push_back 触发的，调用方是 wheel.tick_coro
    // ——已经在 sched 线程上了。所以可以直接 spawn。
    //
    // 不能直接 c->shutdown()，因为 shutdown 是 Task<void> 需要 await；
    // 这里我们在析构里没法 co_await。
    sched->spawn([](TcpConnectionPtr c_) -> Task<void> {
        co_await c_->shutdown();
        co_return;
    }(c));
}

IdleConnectionWheel::IdleConnectionWheel(Scheduler& s, std::chrono::seconds idle)
    : sched_(&s), buckets_(idle.count() > 0 ? (size_t)idle.count() : 1) {
    // 预填 N 个空桶
    size_t N = idle.count() > 0 ? (size_t)idle.count() : 1;
    for (size_t i = 0; i < N; ++i) {
        buckets_.push_back(Bucket{});
    }
}

void IdleConnectionWheel::start() {
    running_.store(true, std::memory_order_relaxed);
    sched_->spawn(tick_coro());
}

std::shared_ptr<IdleEntry> IdleConnectionWheel::register_conn(
    std::weak_ptr<TcpConnection> c) {
    // 注意：使用显式构造函数，避免 IdleEntry{...} 聚合初始化产生临时对象，
    // 该临时对象的析构会被 ~IdleEntry 误判为"超时淘汰"，从而立即关闭新连接。
    auto e = std::make_shared<IdleEntry>(std::move(c), sched_);
    buckets_.back().insert(e);
    return e;
}

void IdleConnectionWheel::refresh(const std::shared_ptr<IdleEntry>& e) {
    if (e) buckets_.back().insert(e);
}

Task<void> IdleConnectionWheel::tick_coro() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_relaxed)) {
        co_await TimeoutAwaiter{1s, *sched_};
        if (!running_.load(std::memory_order_relaxed)) break;
        // push_back 空桶：覆盖最旧桶 → 旧桶中所有 shared_ptr<IdleEntry>
        // 引用计数减一；若该 entry 不再被其它桶持有 → ~IdleEntry → 关闭连接
        buckets_.push_back(Bucket{});
    }
    co_return;
}

}  // namespace coro_net
