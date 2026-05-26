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
// 【内部结构】
//
//   ┌──────────── TcpServer ────────────┐
//   │   listen_fd (主线程绑定)            │
//   │   SchedulerPool (N workers)        │
//   │   ┌──────┐ ┌──────┐ ┌──────┐       │
//   │   │ W[0] │ │ W[1] │ │ W[N-1]│      │
//   │   │ ring │ │ ring │ │ ring │       │
//   │   │ wheel│ │ wheel│ │ wheel│       │
//   │   └──────┘ └──────┘ └──────┘       │
//   │     ▲                              │
//   │     │ accept 协程跑在 W[0] 上       │
//   │     │ 新连接 round-robin 派给       │
//   │     │ 其他 worker 跑 handler        │
//   └────────────────────────────────────┘
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
