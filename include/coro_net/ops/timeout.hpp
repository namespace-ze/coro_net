// =============================================================================
// coro_net/ops/timeout.hpp —— TimeoutAwaiter
// =============================================================================
// 用法：
//     co_await TimeoutAwaiter{std::chrono::milliseconds(500)};
//
// 实现：用 IORING_OP_TIMEOUT，参数是 __kernel_timespec。
// 注意：timespec 必须在 IO 完成前一直有效——放在 awaiter 成员里即可。
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <chrono>

namespace coro_net {

class TimeoutAwaiter : public IoOperationBase {
public:
    TimeoutAwaiter(std::chrono::nanoseconds ns,
                   Scheduler& s = *Scheduler::current())
        : IoOperationBase(s) {
        ts_.tv_sec = ns.count() / 1'000'000'000;
        ts_.tv_nsec = ns.count() % 1'000'000'000;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        // 第 3 个参数 count = 0 表示纯时间触发（不等待任何 CQE 累积）
        // 第 4 个参数 flags = 0 表示相对时间（与现在比，TIMEOUT_ABS 则是绝对时间）
        io_uring_prep_timeout(sqe, &ts_, 0, 0);
        prepare_common(sqe, h);
    }

    // 注意：timeout 触发时 res 是 -ETIME（正常完成），调用者一般忽略。
    int await_resume() noexcept { return result_; }

private:
    __kernel_timespec ts_{};
};

}  // namespace coro_net
