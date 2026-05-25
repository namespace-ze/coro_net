// =============================================================================
// test_tcp_server.cc — TcpServer 端到端 + 时间轮
// =============================================================================
// 测试 1：基本 echo（4 worker + 20 连接）
// 测试 2：idle_timeout 起作用：连上不发数据，超时后 server 主动断
// =============================================================================

#include "coro_net/tcp.hpp"
#include "test_util.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace coro_net;
using namespace std::chrono_literals;

// ---------- 工具：选一个空闲端口（让内核 bind:0 后读回端口） ----------
static uint16_t pick_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, (sockaddr*)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(fd, (sockaddr*)&a, &l);
    uint16_t port = ntohs(a.sin_port);
    close(fd);
    return port;
}

// -----------------------------------------------------------------------------
// 1. 基本 echo
// -----------------------------------------------------------------------------
CORO_TEST(tcp_server_echo) {
    constexpr int N_CONN = 20;
    uint16_t port = pick_port();

    TcpServer server(InetAddress{port, "127.0.0.1"}, /*workers=*/4);

    std::atomic<int> connections{0};
    server.set_handler([&](TcpConnectionPtr conn) -> Task<void> {
        Buffer buf;
        while (true) {
            ssize_t n = co_await conn->recv(buf);
            if (n <= 0) break;
            std::string s = buf.retrieveAllAsString();
            co_await conn->send(s);
        }
        co_await conn->shutdown();
        co_return;
    });

    server.start();

    // client 线程组
    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < N_CONN; ++i) {
        clients.emplace_back([port, i, &ok]() {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port = htons(port);
            for (int r = 0; r < 100; ++r) {
                if (connect(fd, (sockaddr*)&a, sizeof(a)) == 0) break;
                std::this_thread::sleep_for(20ms);
            }
            char msg[32];
            std::snprintf(msg, sizeof(msg), "hello-%d", i);
            size_t mlen = std::strlen(msg);
            send(fd, msg, mlen, 0);
            char buf[64] = {};
            ssize_t r = recv(fd, buf, sizeof(buf), 0);
            if (r == (ssize_t)mlen && std::memcmp(buf, msg, mlen) == 0) {
                ok.fetch_add(1);
            }
            ::shutdown(fd, SHUT_WR);
            // 等待 server 关闭
            char tmp[16];
            recv(fd, tmp, sizeof(tmp), 0);
            close(fd);
        });
    }
    for (auto& t : clients) t.join();

    server.stop();
    server.wait();

    CORO_EXPECT_EQ(ok.load(), N_CONN);
}

// -----------------------------------------------------------------------------
// 2. 空闲超时：连上不发数据，2 秒后应被服务端踢
// -----------------------------------------------------------------------------
CORO_TEST(tcp_server_idle_timeout) {
    uint16_t port = pick_port();

    TcpServer server(InetAddress{port, "127.0.0.1"}, /*workers=*/2);
    server.set_idle_timeout(2s);
    server.set_handler([](TcpConnectionPtr conn) -> Task<void> {
        Buffer buf;
        while (true) {
            ssize_t n = co_await conn->recv(buf);
            if (n <= 0) break;
        }
        co_return;
    });
    server.start();

    // 客户端连上但不发任何数据
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    int conn_r = -1;
    for (int r = 0; r < 100; ++r) {
        conn_r = connect(cfd, (sockaddr*)&a, sizeof(a));
        if (conn_r == 0) break;
        std::this_thread::sleep_for(20ms);
    }
    CORO_EXPECT_EQ(conn_r, 0);

    // 等 3 秒（idle 2 秒 + 一点裕量）
    std::this_thread::sleep_for(3500ms);

    // 此时 client 端 recv 应该返回 0（对端关闭）
    char buf[16];
    ssize_t r = recv(cfd, buf, sizeof(buf), 0);
    CORO_EXPECT_EQ(r, (ssize_t)0);

    close(cfd);
    server.stop();
    server.wait();
}

int main() { return coro_test::run_all(); }
