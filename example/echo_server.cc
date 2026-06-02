// =============================================================================
// example/echo_server.cc —— coro_net 协程版 echo server
// =============================================================================
// 与 mymuduo 的 bench_echo_server 在功能上 1:1 对应，
// 用法: echo_server_coro <port> <workers>
//   port    : 监听端口（默认 8002）
//   workers : worker 线程数（默认 4）
// =============================================================================

#include "coro_net/tcp.hpp"
#include "coro_net/log.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>

static coro_net::TcpServer* g_server = nullptr;

static void on_signal(int) {
    if (g_server) g_server->stop();
}

// 读 unsigned 环境变量（缺省/非法 → dflt）。用于压测时免重编调优。
static unsigned env_u(const char* key, unsigned dflt) {
    const char* v = std::getenv(key);
    if (!v || !*v) return dflt;
    char* end = nullptr;
    unsigned long r = std::strtoul(v, &end, 10);
    return (end && *end == '\0') ? static_cast<unsigned>(r) : dflt;
}

int main(int argc, char** argv) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8002;
    int      nthr = (argc > 2) ? std::stoi(argv[2]) : 4;

    // 调优旋钮（环境变量，压测时无需重编；详见 benchmark/REPRODUCE.md §3.1）：
    //   CORO_SQPOLL_THREADS：SQPOLL 轮询线程数 M（0=关闭 SQPOLL，默认 4）
    //   CORO_FIXED_BUFFERS ：1=启用固定注册缓冲池（默认 1），0=关闭走堆 Buffer
    //   CORO_BUF_CAPACITY  ：每 worker slot 数（0=默认 kDefaultBufCapacity；
    //                        须 ≥ ceil(目标并发/workers) 且 ≤ 16384）
    //   CORO_BUF_SLOT_SIZE ：每 slot 字节（默认 16384）
    const unsigned sqpoll_m = env_u("CORO_SQPOLL_THREADS", 4);
    const bool     fixed    = env_u("CORO_FIXED_BUFFERS", 1) != 0;
    const unsigned buf_cap  = env_u("CORO_BUF_CAPACITY", 0);
    const unsigned slot_sz  = env_u("CORO_BUF_SLOT_SIZE", 16 * 1024);

    coro_net::init_logger("echo_server_coro");
    LOG_INFO << "listen :" << port << " workers=" << nthr
             << " sqpoll_threads=" << sqpoll_m
             << " fixed_buffers=" << (fixed ? 1 : 0)
             << " buf_capacity=" << buf_cap << " slot_size=" << slot_sz;

    coro_net::TcpServer server(
        coro_net::InetAddress{port, "0.0.0.0"},
        static_cast<size_t>(nthr));

    server.set_sqpoll_threads(sqpoll_m);
    server.set_fixed_buffers(fixed, slot_sz, buf_cap);

    server.set_handler([](coro_net::TcpConnectionPtr conn)
                       -> coro_net::Task<void> {
        if (conn->has_fixed_slot()) {
            // 固定注册缓冲零拷贝路径：read_fixed 进 slot，再从同一 slot write_fixed。
            while (true) {
                auto v = co_await conn->recv_fixed();
                if (v.n <= 0) break;
                ssize_t s = co_await conn->send_fixed(v.data,
                                                      static_cast<size_t>(v.n));
                if (s < 0) break;
            }
        } else {
            // 回退路径（无固定缓冲池：未启用 / 池满 / memlock 不足）。
            coro_net::Buffer buf;
            while (true) {
                ssize_t n = co_await conn->recv(buf);
                if (n <= 0) break;
                std::string s = buf.retrieveAllAsString();
                co_await conn->send(s);
            }
        }
        co_await conn->shutdown();
        co_return;
    });

    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    server.start();

    // 心跳：每 5 秒在 worker[0] 上打一行（演示 Timer）
    // server.pool().at(0).run_every(std::chrono::seconds(5), [] {
    //     LOG_INFO << "heartbeat";
    // });

    server.wait();
    LOG_INFO << "exited";
    coro_net::shutdown_logger();
    return 0;
}
