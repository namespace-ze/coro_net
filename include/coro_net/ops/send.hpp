// =============================================================================
// coro_net/ops/send.hpp —— SendAwaiter（single-shot 普通 send）
// =============================================================================
// 注意：一次 send 不保证发完所有字节。生产代码应在外层循环里检查 result_
// 并对 buf+result_、len-result_ 继续 send。本 awaiter 不做循环（保持简单）。
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace coro_net {

class SendAwaiter : public IoOperationBase {
public:
    SendAwaiter(int fd, const void* buf, size_t len,
                Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(buf), len_(len) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        // MSG_NOSIGNAL：对端断开时不投递 SIGPIPE，而是返回 -EPIPE
        io_uring_prep_send(sqe, fd_, buf_, len_, MSG_NOSIGNAL);
        prepare_common(sqe, h);
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int fd_;
    const void* buf_;
    size_t len_;
};

}  // namespace coro_net
