// =============================================================================
// coro_net/ops/recv_fixed.hpp —— RecvFixedAwaiter（固定注册缓冲 read_fixed）
// =============================================================================
// 用 io_uring_prep_read_fixed 把数据直接收进一个已注册的固定 slot（零拷贝，
// 内核免去每次 IO 的 get_user_pages）。buf 必须落在 buf_index 指向的注册缓冲内。
//
// read 作用于已连接 TCP 流套接字时语义等同 recv(flags=0)。
//
// 【返回值】(await_resume)
//   > 0：本次收到的字节数（已写入 slot 起始处）
//   = 0：对端关闭
//   < 0：-errno
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <sys/types.h>

namespace coro_net {

class RecvFixedAwaiter : public IoOperationBase {
public:
    RecvFixedAwaiter(int fd, void* buf, unsigned len, int buf_index,
                     Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(buf), len_(len),
          buf_index_(buf_index) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        io_uring_prep_read_fixed(sqe, fd_, buf_, len_, /*offset=*/0, buf_index_);
        prepare_common(sqe, h);
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int      fd_;
    void*    buf_;
    unsigned len_;
    int      buf_index_;
};

}  // namespace coro_net
