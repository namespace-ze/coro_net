// =============================================================================
// test_scheduler_pool.cc — SchedulerPool + 跨 worker MSG_RING + 多线程 echo
// =============================================================================
//
// 测试 1: 跨 worker post_task
//   worker[0] 启动后立刻给 worker[1] post 一个任务，验证 worker[1] 上能跑
//   到该任务且原子变量被设置。
//
// 测试 2: 跨 worker post(coroutine_handle)
//   worker[0] 启动一个协程，让它 co_await TimeoutAwaiter（挂起）；
//   然后 worker[0] 自己 post 给 worker[1] 让 worker[1] post 协程 handle 回
//   worker[0]，验证协程能跨线程被唤醒。
//   （现实中协程不会跨线程 resume；这是测试机制，handle 来源仍是 worker[0]）。
//
// 测试 3: 多线程 echo 服务
//   worker[0] 跑 accept 循环（main thread 发起 N 个 TCP 连接）；
//   每收到一个连接 round-robin 分给其他 worker 跑 echo handler。
//   验证 N 个 client 都收到 echo 数据正确。
// =============================================================================

#include "coro_net/scheduler.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/task.hpp"
#include "test_util.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>

using namespace coro_net;
using namespace std::chrono_literals;

// -----------------------------------------------------------------------------
// 测试 1: 跨 worker post_task
// -----------------------------------------------------------------------------
CORO_TEST(pool_cross_thread_post_task) {
    SchedulerPool pool(2);

    std::atomic<int> counter{0};

    // worker[0] 的 boot task：给 worker[1] 派一个递增任务
    pool.at(0).queue_boot_task([&pool, &counter]() {
        Scheduler* w1 = &pool.at(1);
        w1->post_task([&counter, w1]() {
            counter.fetch_add(1, std::memory_order_relaxed);
            w1->stop();  // 让 worker[1] 结束循环
        });
        // worker[0] 自己也结束
        Scheduler::current()->stop();
    });

    pool.start();
    pool.wait();

    CORO_EXPECT_EQ(counter.load(), 1);
}

// -----------------------------------------------------------------------------
// 测试 2: 多 worker echo（accept 在 worker[0]，handler 在其它 worker）
// -----------------------------------------------------------------------------
static int make_listen_loopback(uint16_t& port_out) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 64) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    port_out = ntohs(addr.sin_port);
    return fd;
}

static Task<void> echo_handler(int conn_fd, Scheduler& s,
                               std::atomic<int>& done_counter,
                               int total_expected,
                               SchedulerPool* pool) {
    char buf[256];
    ssize_t n = co_await RecvAwaiter{conn_fd, buf, sizeof(buf), s};
    if (n > 0) {
        co_await SendAwaiter{conn_fd, buf, (size_t)n, s};
    }
    co_await ShutdownAwaiter{conn_fd, SHUT_WR, s};
    close(conn_fd);

    int done = done_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (done >= total_expected) {
        // 通知所有 worker 停止：每个 worker 自己停自己（用 post_task 兼容跨线程）
        for (size_t i = 0; i < pool->size(); ++i) {
            Scheduler* w = &pool->at(i);
            w->post_task([w]() { w->stop(); });
        }
    }
    co_return;
}

CORO_TEST(pool_multithread_echo) {
    constexpr int N_CONN = 10;
    constexpr int N_WORKER = 4;

    uint16_t port = 0;
    int listen_fd = make_listen_loopback(port);
    CORO_EXPECT_TRUE(listen_fd >= 0);

    SchedulerPool pool(N_WORKER);
    std::atomic<int> done_counter{0};

    // worker[0] 跑 accept loop（single-shot 循环，accept N_CONN 个连接后停）
    pool.at(0).queue_boot_task([&pool, listen_fd, &done_counter]() {
        Scheduler::current()->spawn([](SchedulerPool& pool, int lfd,
                                       std::atomic<int>& done) -> Task<void> {
            Scheduler& self = *Scheduler::current();
            for (int i = 0; i < N_CONN; ++i) {
                AcceptAwaiter aw{lfd, self};
                int conn = co_await aw;
                if (conn < 0) co_return;
                // round-robin 选一个 worker（跳过 worker[0]，避免 acceptor 自己处理）
                size_t idx = 1 + (i % (pool.size() - 1));
                Scheduler* target = &pool.at(idx);
                target->post_task([conn, target, &pool, &done]() {
                    Scheduler::current()->spawn(
                        echo_handler(conn, *target, done, N_CONN, &pool));
                });
            }
            co_return;
        }(pool, listen_fd, done_counter));
    });

    pool.start();

    // 主线程：起 N 个 client，并发连接 + send + recv + 验证
    std::vector<std::thread> clients;
    std::atomic<int> client_ok{0};
    for (int i = 0; i < N_CONN; ++i) {
        clients.emplace_back([port, i, &client_ok]() {
            int cfd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            for (int r = 0; r < 50; ++r) {
                if (connect(cfd, (sockaddr*)&addr, sizeof(addr)) == 0) break;
                std::this_thread::sleep_for(10ms);
            }
            char msg[16];
            std::snprintf(msg, sizeof(msg), "hi%d", i);
            size_t mlen = std::strlen(msg);
            send(cfd, msg, mlen, 0);
            char buf[32] = {};
            ssize_t r = recv(cfd, buf, sizeof(buf), 0);
            if (r == (ssize_t)mlen && std::memcmp(buf, msg, mlen) == 0) {
                client_ok.fetch_add(1);
            }
            close(cfd);
        });
    }
    for (auto& t : clients) t.join();

    pool.wait();
    close(listen_fd);

    CORO_EXPECT_EQ(client_ok.load(), N_CONN);
}

int main() { return coro_test::run_all(); }
