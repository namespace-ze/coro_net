// =============================================================================
// test_scheduler.cc — Scheduler + awaiter 端到端测试（单线程）
// =============================================================================
// 验证：
//   1. timeout_awaiter 能挂起并按时唤醒
//   2. socketpair 上 send/recv 协程版能跑通（一端发，另一端收，验证内容）
//   3. multiple coroutines 并发跑（一个 server 协程 + 一个 client 协程
//      在同一 Scheduler 上，验证 echo 数据正确）
// =============================================================================

#include "coro_net/scheduler.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/task.hpp"
#include "test_util.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

using namespace coro_net;
using namespace std::chrono_literals;

// -----------------------------------------------------------------------------
// 1. TimeoutAwaiter
// -----------------------------------------------------------------------------
CORO_TEST(scheduler_timeout) {
    Scheduler sched;
    auto start = std::chrono::steady_clock::now();

    sched.spawn([](Scheduler& s) -> Task<void> {
        co_await TimeoutAwaiter{10ms, s};
        s.stop();
    }(sched));

    sched.run();

    auto elapsed = std::chrono::steady_clock::now() - start;
    CORO_EXPECT_TRUE(elapsed >= 10ms);
    CORO_EXPECT_TRUE(elapsed < 200ms);  // 不应该 hang 很久
}

// -----------------------------------------------------------------------------
// 2. socketpair echo（同一 Scheduler 上两个协程）
// -----------------------------------------------------------------------------
CORO_TEST(scheduler_socketpair_echo) {
    Scheduler sched;
    int fds[2];
    CORO_EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    // server 协程：recv 一帧 → send 回去
    auto server_coro = [](Scheduler& s, int fd) -> Task<void> {
        char buf[64] = {};
        ssize_t n = co_await RecvAwaiter{fd, buf, sizeof(buf), s};
        CORO_EXPECT_EQ(n, (ssize_t)5);  // "hello"
        CORO_EXPECT_EQ(std::strncmp(buf, "hello", 5), 0);

        ssize_t m = co_await SendAwaiter{fd, "world", 5, s};
        CORO_EXPECT_EQ(m, (ssize_t)5);
        co_return;
    };

    // client 协程：send → recv → 验证 → stop
    auto client_coro = [](Scheduler& s, int fd) -> Task<void> {
        ssize_t m = co_await SendAwaiter{fd, "hello", 5, s};
        CORO_EXPECT_EQ(m, (ssize_t)5);

        char buf[64] = {};
        ssize_t n = co_await RecvAwaiter{fd, buf, sizeof(buf), s};
        CORO_EXPECT_EQ(n, (ssize_t)5);
        CORO_EXPECT_EQ(std::strncmp(buf, "world", 5), 0);

        s.stop();
        co_return;
    };

    sched.spawn(server_coro(sched, fds[0]));
    sched.spawn(client_coro(sched, fds[1]));
    sched.run();

    close(fds[0]);
    close(fds[1]);
}

// -----------------------------------------------------------------------------
// 3. ShutdownAwaiter：发起 SHUT_WR 后对端 recv 返回 0
// -----------------------------------------------------------------------------
CORO_TEST(scheduler_shutdown) {
    Scheduler sched;
    int fds[2];
    CORO_EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    auto sender = [](Scheduler& s, int fd) -> Task<void> {
        // 立刻关闭写端
        co_await ShutdownAwaiter{fd, SHUT_WR, s};
        co_return;
    };

    auto receiver = [](Scheduler& s, int fd) -> Task<void> {
        char buf[16];
        ssize_t n = co_await RecvAwaiter{fd, buf, sizeof(buf), s};
        CORO_EXPECT_EQ(n, (ssize_t)0);  // 对端 EOF
        s.stop();
        co_return;
    };

    sched.spawn(sender(sched, fds[0]));
    sched.spawn(receiver(sched, fds[1]));
    sched.run();

    close(fds[0]);
    close(fds[1]);
}

int main() { return coro_test::run_all(); }
