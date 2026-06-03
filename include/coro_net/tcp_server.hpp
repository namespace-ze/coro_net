// =============================================================================
// coro_net/tcp_server.hpp —— 用户视角的网络服务入口
// =============================================================================
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
//   server.wait();
//
// 【内部结构】shared-nothing：每 worker 独立 listen(SO_REUSEPORT) + 本地 accept
//
//   ┌──────────────── TcpServer ────────────────┐
//   │   SchedulerPool (N workers)                │
//   │   ┌────────┐ ┌────────┐ ┌────────┐         │
//   │   │ W[0]   │ │ W[1]   │ │ W[N-1] │         │
//   │   │ listen │ │ listen │ │ listen │ ← 同端口 SO_REUSEPORT
//   │   │ ring   │ │ ring   │ │ ring   │         │
//   │   │ wheel  │ │ wheel  │ │ wheel  │         │
//   │   │ bufpool│ │ bufpool│ │ bufpool│ ← 固定注册缓冲池
//   │   └────────┘ └────────┘ └────────┘         │
//   │     每个 worker 各自 accept、各自跑 handler  │
//   │     内核按四元组哈希把新连接分到各 listen fd  │
//   │     连接不跨线程迁移（无 post_task 派发）     │
//   └────────────────────────────────────────────┘
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/inet_address.hpp"
#include "coro_net/scheduler_pool.hpp"
#include "coro_net/idle_connection_wheel.hpp"
#include "coro_net/tcp_connection.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace coro_net {

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

    // -------------------------------------------------------------------------
    // 性能调优（须在 start() 之前调用）
    // -------------------------------------------------------------------------
    // SQPOLL 轮询线程数 M：0 关闭 SQPOLL；>0 时把 N 个 ring 分 M 组共享 M 个
    // 轮询 kthread（默认 4）。详见 SchedulerPool / IoUring。
    void set_sqpoll_threads(unsigned m) { sqpoll_threads_ = m; }

    // 把 M 个 SQPOLL 轮询线程钉到指定核（第 g 个轮询线程 → cpus[g]）。
    // 空 = 不钉核。给轮询线程专属核（并避开网络 softirq 核）可消除吞吐抖动。
    void set_sqpoll_cpus(std::vector<int> cpus) { sqpoll_cpus_ = std::move(cpus); }

    // 固定注册缓冲池：on=true 时每 worker 注册 capacity_per_worker 个
    // slot_size 字节的固定缓冲，连接用 read_fixed/write_fixed 零拷贝收发。
    // capacity_per_worker=0 用默认（kDefaultBufCapacity）。
    // 注意：固定缓冲被 pin，须保证 RLIMIT_MEMLOCK ≥ capacity*slot_size*workers；
    // 不足时自动回退普通 recv/send。capacity 须 ≥ ceil(目标并发/workers) 且 ≤16384。
    static constexpr unsigned kDefaultBufCapacity = 4096;
    void set_fixed_buffers(bool on, unsigned slot_size = 16 * 1024,
                           unsigned capacity_per_worker = 0) {
        use_fixed_buffers_ = on;
        buf_slot_size_ = slot_size;
        buf_pool_capacity_ = capacity_per_worker;
    }

    // 启动：bind + listen + 拉起 worker 线程；不阻塞
    void start();

    // 通知停止；尝试唤醒所有 worker
    void stop();

    // 等待所有 worker 退出
    void wait();

    SchedulerPool& pool() noexcept { return *pool_; }

private:
    InetAddress addr_;
    size_t worker_threads_;                 // worker 数（pool 在 start() 延迟构造）
    std::vector<int> listen_fds_;           // 每 worker 一个 SO_REUSEPORT listen fd
    std::unique_ptr<SchedulerPool> pool_;   // start() 时按配置构造
    Handler handler_;
    std::chrono::seconds idle_{0};
    std::vector<std::unique_ptr<IdleConnectionWheel>> wheels_;
    std::atomic_bool running_{false};

    // 调优参数（start() 前设置）
    unsigned sqpoll_threads_ = 4;           // M（0=不开 SQPOLL）
    std::vector<int> sqpoll_cpus_;          // SQPOLL 轮询线程钉核（空=不钉）
    bool     use_fixed_buffers_ = true;
    unsigned buf_slot_size_ = 16 * 1024;
    unsigned buf_pool_capacity_ = 0;        // 0=用 kDefaultBufCapacity
};

}  // namespace coro_net
