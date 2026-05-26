// =============================================================================
// coro_net/ops/shutdown.hpp —— ShutdownAwaiter（半关闭连接）
// =============================================================================
// 用法：
//     co_await ShutdownAwaiter{fd, SHUT_WR};
//     close(fd);            // 半关闭后通常配合 close
//
// SHUT_RD：禁止接收
// SHUT_WR：禁止发送（发送 FIN，对端 read 返回 0）
// SHUT_RDWR：双向关闭
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>

namespace coro_net {

class ShutdownAwaiter : public IoOperationBase {
public:
    ShutdownAwaiter(int fd, int how,
                    Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), how_(how) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        io_uring_prep_shutdown(sqe, fd_, how_);
        prepare_common(sqe, h);
    }

    int await_resume() noexcept { return result_; }

private:
    int fd_;
    int how_;
};

}  // namespace coro_net
