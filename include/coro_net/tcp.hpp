// =============================================================================
// coro_net/tcp.hpp —— TcpServer / TcpConnection / 时间轮
// =============================================================================
//
// 【面向用户的核心入口】
//
//   coro_net::TcpServer server({8080}, /*worker_threads=*/4);
//   server.set_idle_timeout(std::chrono::seconds(60));
//   server.set_handler([](auto conn) -> coro_net::Task<void> {
//       coro_net::Buffer buf;
//       for (;;) {
//           ssize_t n = co_await conn->recv(buf);
//           if (n == 0) break;                                  // 对端关闭
//           if (n < 0) break;                                   // 错误
//           // ……拆帧、处理、send ……
//           std::string echo = buf.retrieveAllAsString();
//           co_await conn->send(echo);
//       }
//       co_return;
//   });
//   server.start();
//   server.wait();   // 阻塞至 stop
//
// 【内部结构】
//
//   ┌──────────── TcpServer ────────────┐
//   │                                    │
//   │   listen_fd (主线程绑定)            │
//   │                                    │
//   │   SchedulerPool (N workers)        │
//   │   ┌──────┐ ┌──────┐ ┌──────┐       │
//   │   │ W[0] │ │ W[1] │ │ W[N-1]│       │
//   │   │ ring │ │ ring │ │ ring │       │
//   │   │ wheel│ │ wheel│ │ wheel│       │  每 worker 一个时间轮
//   │   └──────┘ └──────┘ └──────┘       │
//   │     ▲                              │
//   │     │ accept 协程跑在 W[0] 上       │
//   │     │ 新连接 round-robin 派给       │
//   │     │ 其他 worker 跑 handler        │
//   └────────────────────────────────────┘
//
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/buffer.hpp"
#include "coro_net/inet_address.hpp"
#include "coro_net/ops.hpp"
#include "../../src/util/circular_buffer.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <unordered_set>
#include <span>
#include <any>
#include <thread>
#include <unistd.h>

namespace coro_net {

class IdleConnectionWheel;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// -----------------------------------------------------------------------------
// IdleEntry —— 挂在时间轮某个桶里的标记
// -----------------------------------------------------------------------------
//
// 【生命周期】
//   - shared_ptr<IdleEntry> 仅由 IdleConnectionWheel 的桶持有
//   - TcpConnection 内只保留 weak_ptr<IdleEntry>，避免循环
//   - 每次 recv 后，TcpConnection 用 weak.lock() 拿到 entry，
//     IdleConnectionWheel 把 entry 重新插入"队尾桶" → 续命
//   - 满 idle_seconds 没续命 → entry 在所有桶中的 shared 计数降到 0
//     → ~IdleEntry 触发 → 在 sched 上 spawn 一个关闭协程关闭连接
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// TcpConnection
// -----------------------------------------------------------------------------
//
// 【线程亲和】
//   一个 TcpConnection 整个生命周期内只属于一个 Scheduler（worker）。
//   它的 fd 上所有 IO（recv/send/shutdown）都通过该 Scheduler 的 io_uring 提交。
//   所以 TcpConnection 的成员函数 *只能* 在它所属的 Scheduler 线程上调用。
//
// 【shared_ptr 与所有权】
//   handler 协程的协程帧持有 shared_ptr<TcpConnection>，
//   IdleEntry 持有 weak_ptr，时间轮 bucket 持有 shared_ptr<IdleEntry>。
//   只要 handler 没结束，TcpConnection 就活着；handler co_return 后，
//   shared 计数归零，~TcpConnection 关闭 fd。
// -----------------------------------------------------------------------------
class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(int fd, InetAddress peer, Scheduler& sched)
        : fd_(fd), peer_(peer), sched_(&sched) {}

    ~TcpConnection() {
        if (fd_ >= 0) ::close(fd_);
    }

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // -------------------------------------------------------------------------
    // recv —— 单次异步读
    // -------------------------------------------------------------------------
    // 返回:
    //   > 0: 实际读到的字节（已 append 到 buf 末尾）
    //   = 0: 对端关闭
    //   < 0: -errno
    //
    // 内部用 BufferRing + memcpy（详见 RecvIntoBufferAwaiter），
    // 同时如果 idle_entry_ 还活着，调用 wheel->refresh 续命。
    // -------------------------------------------------------------------------
    Task<ssize_t> recv(Buffer& buf);

    // -------------------------------------------------------------------------
    // send —— 写完全部字节（内部循环 send，直到全发或出错）
    // -------------------------------------------------------------------------
    Task<ssize_t> send(std::span<const char> data);

    // 便捷重载
    Task<ssize_t> send(const std::string& s) {
        return send(std::span<const char>(s.data(), s.size()));
    }

    // -------------------------------------------------------------------------
    // shutdown —— 半关闭写端（让对端 recv 收到 EOF）
    // -------------------------------------------------------------------------
    Task<void> shutdown();

    int fd() const noexcept { return fd_; }
    const InetAddress& peer() const noexcept { return peer_; }
    Scheduler& scheduler() noexcept { return *sched_; }

    void set_context(std::any c) { ctx_ = std::move(c); }
    std::any& context() noexcept { return ctx_; }

    // 由 TcpServer 在 connection 建立后调用，挂上时间轮
    void install_idle(IdleConnectionWheel* w, std::shared_ptr<IdleEntry> e) {
        wheel_ = w;
        idle_entry_ = e;   // 存为 weak_ptr
    }

private:
    int fd_;
    InetAddress peer_;
    Scheduler* sched_;
    std::any ctx_;
    IdleConnectionWheel* wheel_ = nullptr;
    std::weak_ptr<IdleEntry> idle_entry_;
};

// -----------------------------------------------------------------------------
// IdleConnectionWheel —— 每个 worker 持有一个的时间轮
// -----------------------------------------------------------------------------
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

// =============================================================================
// TcpServer —— 用户视角的网络服务入口
// =============================================================================
class TcpServer {
public:
    using Handler = std::function<Task<void>(TcpConnectionPtr)>;

    TcpServer(InetAddress addr,
              size_t worker_threads = std::thread::hardware_concurrency());
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 设置每个新连接到来时执行的协程处理器
    void set_handler(Handler h) { handler_ = std::move(h); }

    // 设置空闲超时秒数；0 关闭该特性
    void set_idle_timeout(std::chrono::seconds s) { idle_ = s; }

    // 启动：bind + listen + 拉起 worker 线程；不阻塞
    void start();

    // 通知停止；尝试唤醒所有 worker
    void stop();

    // 等待所有 worker 退出
    void wait();

    SchedulerPool& pool() noexcept { return pool_; }

private:
    InetAddress addr_;
    int listen_fd_ = -1;
    SchedulerPool pool_;
    Handler handler_;
    std::chrono::seconds idle_{0};
    std::vector<std::unique_ptr<IdleConnectionWheel>> wheels_;
    std::atomic_bool running_{false};
};

}  // namespace coro_net
