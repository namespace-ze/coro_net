// =============================================================================
// tcp_connection.cc — TcpConnection 实现
// =============================================================================
// 单连接的 recv/send/shutdown 协程接口。
// IdleEntry 析构（与时间轮强耦合）放在 idle_connection_wheel.cc。
// =============================================================================
#include "coro_net/tcp.hpp"
#include "coro_net/ops.hpp"

#include <sys/socket.h>

namespace coro_net {

Task<ssize_t> TcpConnection::recv(Buffer& buf) {
    ssize_t n = co_await RecvIntoBufferAwaiter{fd_, buf, *sched_};
    // 续命：每次有数据进来就把自己重新插入队尾桶
    if (n > 0 && wheel_) {
        if (auto e = idle_entry_.lock()) {
            wheel_->refresh(e);
        }
    }
    co_return n;
}

Task<ssize_t> TcpConnection::send(std::span<const char> data) {
    size_t remaining = data.size();
    const char* p = data.data();
    ssize_t total = 0;
    while (remaining > 0) {
        ssize_t n = co_await SendAwaiter{fd_, p, remaining, *sched_};
        if (n < 0) co_return n;
        if (n == 0) co_return total;
        total += n;
        p += n;
        remaining -= n;
    }
    co_return total;
}

Task<void> TcpConnection::shutdown() {
    if (fd_ < 0) co_return;
    co_await ShutdownAwaiter{fd_, SHUT_RDWR, *sched_};
    co_return;
}

}  // namespace coro_net
