// =============================================================================
// coro_net/ops/accept.hpp —— AcceptAwaiter
// =============================================================================
// 用法：
//     int conn_fd = co_await AcceptAwaiter{listen_fd};
//     if (conn_fd < 0) { ... }
//
// 返回值（await_resume）：
//     >= 0 ：新的连接 fd
//     <  0 ：-errno
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstring>

namespace coro_net {

class AcceptAwaiter : public IoOperationBase {
public:
    explicit AcceptAwaiter(int listen_fd, Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), listen_fd_(listen_fd) {
        std::memset(&peer_addr_, 0, sizeof(peer_addr_));
        peer_len_ = sizeof(peer_addr_);
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        // 注意：传入的 addr 与 addrlen 内核会在 IO 完成时填，所以指针必须在
        // awaiter 生命周期内稳定有效——awaiter 在协程帧里，挂起期间地址稳定。
        io_uring_prep_accept(sqe, listen_fd_,
                             reinterpret_cast<sockaddr*>(&peer_addr_),
                             &peer_len_, 0);
        prepare_common(sqe, h);
    }

    int await_resume() noexcept { return result_; }

    const sockaddr_in& peer() const noexcept { return peer_addr_; }
    socklen_t peer_len() const noexcept { return peer_len_; }

private:
    int listen_fd_;
    sockaddr_in peer_addr_{};
    socklen_t peer_len_{};
};

}  // namespace coro_net
