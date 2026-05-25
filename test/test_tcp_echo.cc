// =============================================================================
// test_tcp_echo.cc — TCP loopback echo 测试，验证 AcceptAwaiter + recv + send
// =============================================================================
// 单线程跑：
//   - 主协程：bind/listen → accept 一次拿到 conn_fd → echo 一帧
//   - 辅助：另起一个线程跑同步 client 来连接 / 发数据 / 收回包
//
// 这个测试模拟一次最小化的 echo 服务，覆盖到 single-shot accept 的 awaiter。
// =============================================================================

#include "coro_net/scheduler.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/task.hpp"
#include "test_util.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <thread>
#include <atomic>
#include <cstring>

using namespace coro_net;

static int make_listen(uint16_t& port_out) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);  // 让内核选一个
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 16) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    port_out = ntohs(addr.sin_port);
    return fd;
}

CORO_TEST(tcp_echo_single_shot) {
    uint16_t port = 0;
    int listen_fd = make_listen(port);
    CORO_EXPECT_TRUE(listen_fd >= 0);

    Scheduler sched;

    // 服务端协程：accept → recv → send
    sched.spawn([](Scheduler& s, int lfd) -> Task<void> {
        AcceptAwaiter aw{lfd, s};
        int conn = co_await aw;
        CORO_EXPECT_TRUE(conn >= 0);

        char buf[64] = {};
        ssize_t n = co_await RecvAwaiter{conn, buf, sizeof(buf), s};
        CORO_EXPECT_TRUE(n > 0);

        ssize_t m = co_await SendAwaiter{conn, buf, (size_t)n, s};
        CORO_EXPECT_EQ(m, n);

        co_await ShutdownAwaiter{conn, SHUT_WR, s};
        close(conn);
        s.stop();
        co_return;
    }(sched, listen_fd));

    // 客户端用普通同步线程连
    std::atomic<bool> client_ok{false};
    std::thread client([port, &client_ok]() {
        int cfd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        // 重试几次（避免 server 还没进入 accept 就连）
        for (int i = 0; i < 50; ++i) {
            if (connect(cfd, (sockaddr*)&addr, sizeof(addr)) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const char* msg = "ping";
        ssize_t w = send(cfd, msg, 4, 0);
        if (w != 4) { close(cfd); return; }
        char buf[16] = {};
        ssize_t r = recv(cfd, buf, sizeof(buf), 0);
        client_ok = (r == 4 && std::memcmp(buf, "ping", 4) == 0);
        close(cfd);
    });

    sched.run();
    client.join();
    close(listen_fd);

    CORO_EXPECT_TRUE(client_ok.load());
}

int main() { return coro_test::run_all(); }
