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

int main(int argc, char** argv) {
    uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8002;
    int      nthr = (argc > 2) ? std::stoi(argv[2]) : 4;

    coro_net::init_logger("echo_server_coro");
    LOG_INFO << "listen :" << port << " workers=" << nthr;

    coro_net::TcpServer server(
        coro_net::InetAddress{port, "0.0.0.0"},
        static_cast<size_t>(nthr));

    server.set_handler([](coro_net::TcpConnectionPtr conn)
                       -> coro_net::Task<void> {
        coro_net::Buffer buf;
        while (true) {
            ssize_t n = co_await conn->recv(buf);
            if (n <= 0) break;
            // 整段回显
            std::string s = buf.retrieveAllAsString();
            co_await conn->send(s);
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
