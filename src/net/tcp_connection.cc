// =============================================================================
// tcp_connection.cc — TcpConnection 实现
// =============================================================================
// 单连接的 recv/send/shutdown 协程接口。
// IdleEntry 析构（与时间轮强耦合）放在 idle_connection_wheel.cc。
// =============================================================================
#include "coro_net/tcp.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/registered_buffer_pool.hpp"

#include <sys/socket.h>

namespace coro_net {

TcpConnection::TcpConnection(int fd, InetAddress peer, Scheduler& sched)
    : fd_(fd), peer_(peer), sched_(&sched) {
    // 若所属 worker 有可用固定缓冲池，借一个 slot 作整个生命周期的读写缓冲。
    if (auto* pool = sched_->buffer_pool()) {
        int idx = pool->acquire();
        if (idx >= 0) {
            buf_index_ = idx;
            slot_ = pool->slot_ptr(idx);
            slot_size_ = pool->slot_size();
        }
    }
}

TcpConnection::~TcpConnection() {
    if (buf_index_ >= 0) {
        if (auto* pool = sched_->buffer_pool()) pool->release(buf_index_);
    }
    if (fd_ >= 0) ::close(fd_);
}

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

Task<TcpConnection::RecvView> TcpConnection::recv_fixed() {
    ssize_t n = co_await RecvFixedAwaiter{
        fd_, slot_, static_cast<unsigned>(slot_size_), buf_index_, *sched_};
    // 续命：有数据进来就把自己重新插入队尾桶（与 recv(Buffer&) 一致）
    if (n > 0 && wheel_) {
        if (auto e = idle_entry_.lock()) {
            wheel_->refresh(e);
        }
    }
    co_return RecvView{n, slot_};
}

Task<ssize_t> TcpConnection::send_fixed(const char* data, size_t len) {
    size_t remaining = len;
    const char* p = data;
    ssize_t total = 0;
    while (remaining > 0) {
        ssize_t n = co_await SendFixedAwaiter{
            fd_, p, static_cast<unsigned>(remaining), buf_index_, *sched_};
        if (n < 0) co_return n;
        if (n == 0) co_return total;
        total += n;
        p += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return total;
}

Task<void> TcpConnection::shutdown() {
    if (fd_ < 0) co_return;
    co_await ShutdownAwaiter{fd_, SHUT_RDWR, *sched_};
    co_return;
}

}  // namespace coro_net
