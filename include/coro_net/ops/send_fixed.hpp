// =============================================================================
// coro_net/ops/send_fixed.hpp —— SendFixedAwaiter（固定注册缓冲 write_fixed）
// =============================================================================
// 用 io_uring_prep_write_fixed 从一个已注册的固定 slot 发送（零拷贝）。
// buf 必须落在 buf_index 指向的注册缓冲内（允许是 slot 内的偏移地址，内核校验）。
//
// write 作用于流套接字时语义等同 send(flags=0)，但 *不带 MSG_NOSIGNAL*：
// 对端断开写入会触发 SIGPIPE，调用方（TcpServer）须先 signal(SIGPIPE, SIG_IGN)。
//
// 一次 write 不保证发完，外层 TcpConnection::send_fixed 会循环续发。
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <sys/types.h>

namespace coro_net {

class SendFixedAwaiter : public IoOperationBase {
public:
    SendFixedAwaiter(int fd, const void* buf, unsigned len, int buf_index,
                     Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(buf), len_(len),
          buf_index_(buf_index) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        io_uring_prep_write_fixed(sqe, fd_, buf_, len_, /*offset=*/0, buf_index_);
        prepare_common(sqe, h);
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int         fd_;
    const void* buf_;
    unsigned    len_;
    int         buf_index_;
};

}  // namespace coro_net
