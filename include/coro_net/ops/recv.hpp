// =============================================================================
// coro_net/ops/recv.hpp —— RecvAwaiter（single-shot 普通 recv）
// =============================================================================
// 用法：
//     char buf[4096];
//     ssize_t n = co_await RecvAwaiter{fd, buf, sizeof(buf)};
//     n > 0 ：实际读到的字节
//     n == 0：对端正常关闭
//     n < 0 ：-errno
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <sys/types.h>

namespace coro_net {

class RecvAwaiter : public IoOperationBase {
public:
    RecvAwaiter(int fd, void* buf, size_t len,
                Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(buf), len_(len) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        // flags 第 4 个参数：可以用 MSG_NOSIGNAL 等；这里给 0
        io_uring_prep_recv(sqe, fd_, buf_, len_, 0);
        prepare_common(sqe, h);
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int fd_;
    void* buf_;
    size_t len_;
};

}  // namespace coro_net
