// =============================================================================
// test_thread_pool.cc —— CoroThreadPool + 协程衔接
// =============================================================================
// 测试 1：基本 submit，业务线程返回值，协程在原 worker resume
// 测试 2：异常透传
// 测试 3：与 TcpServer 一起跑 —— echo 服务在业务线程上做一次 sleep 模拟业务
// =============================================================================

#include "coro_net/thread_pool.hpp"
#include "coro_net/tcp.hpp"
#include "test_util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <stdexcept>

using namespace coro_net;
using namespace std::chrono_literals;

static uint16_t pick_port_t() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, (sockaddr*)&a, sizeof(a));
    socklen_t l = sizeof(a);
    getsockname(fd, (sockaddr*)&a, &l);
    uint16_t p = ntohs(a.sin_port);
    close(fd);
    return p;
}

// -----------------------------------------------------------------------------
// 1. submit + 协程协同（无 TCP）
// -----------------------------------------------------------------------------
CORO_TEST(thread_pool_submit_value) {
    Scheduler sched;
    CoroThreadPool pool("test-pool", 2);
    pool.start();

    int result = 0;
    sched.queue_boot_task([&]() {
        Scheduler::current()->spawn([](Scheduler& s, CoroThreadPool& p,
                                       int& r) -> Task<void> {
            int x = co_await p.submit([] {
                std::this_thread::sleep_for(10ms);
                return 42;
            });
            r = x;
            s.stop();
            co_return;
        }(*Scheduler::current(), pool, result));
    });

    sched.run();
    pool.stop();
    CORO_EXPECT_EQ(result, 42);
}

// -----------------------------------------------------------------------------
// 2. 异常透传
// -----------------------------------------------------------------------------
CORO_TEST(thread_pool_submit_exception) {
    Scheduler sched;
    CoroThreadPool pool("test-pool", 2);
    pool.start();

    bool caught = false;
    sched.queue_boot_task([&]() {
        Scheduler::current()->spawn([](Scheduler& s, CoroThreadPool& p,
                                       bool& c) -> Task<void> {
            try {
                co_await p.submit([]() -> int {
                    throw std::runtime_error("biz error");
                });
            } catch (const std::runtime_error& e) {
                c = (std::string(e.what()) == "biz error");
            }
            s.stop();
            co_return;
        }(*Scheduler::current(), pool, caught));
    });

    sched.run();
    pool.stop();
    CORO_EXPECT_TRUE(caught);
}

// -----------------------------------------------------------------------------
// 3. TcpServer + 业务线程池
// -----------------------------------------------------------------------------
CORO_TEST(thread_pool_with_tcp_server) {
    constexpr int N_CONN = 8;
    uint16_t port = pick_port_t();

    CoroThreadPool biz("biz", 4);
    biz.start();

    TcpServer server(InetAddress{port, "127.0.0.1"}, /*workers=*/2);
    server.set_handler([&biz](TcpConnectionPtr conn) -> Task<void> {
        Buffer buf;
        while (true) {
            ssize_t n = co_await conn->recv(buf);
            if (n <= 0) break;
            std::string s = buf.retrieveAllAsString();
            // 把"业务"丢到业务线程池：模拟 1ms 计算
            std::string r = co_await biz.submit([s]() {
                std::this_thread::sleep_for(1ms);
                return std::string("re:") + s;
            });
            co_await conn->send(r);
        }
        co_await conn->shutdown();
        co_return;
    });
    server.start();

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
            std::string msg = "ping-" + std::to_string(i);
            send(fd, msg.data(), msg.size(), 0);
            char buf[64] = {};
            ssize_t r = recv(fd, buf, sizeof(buf), 0);
            std::string expected = "re:" + msg;
            if (r == (ssize_t)expected.size() &&
                std::memcmp(buf, expected.data(), expected.size()) == 0) {
                ok.fetch_add(1);
            }
            ::shutdown(fd, SHUT_WR);
            char tmp[16]; recv(fd, tmp, sizeof(tmp), 0);
            close(fd);
        });
    }
    for (auto& t : clients) t.join();

    server.stop();
    server.wait();
    biz.stop();

    CORO_EXPECT_EQ(ok.load(), N_CONN);
}

int main() { return coro_test::run_all(); }
