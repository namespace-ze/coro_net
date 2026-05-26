// =============================================================================
// coro_net/ops/recv_into_buffer.hpp —— RecvIntoBufferAwaiter
// =============================================================================
// 让 io_uring 直接把数据写到调用者的 per-conn Buffer，不经 BufferRing 中转。
//
// 【设计取舍】
//   - 优点：少一次 memcpy（约 200ns/4KB）
//   - 代价：内存按连接独立持有（10K 连接 × 1KB ≈ 10MB/worker）
//
// 历史上这里用 IOSQE_BUFFER_SELECT + BufferRing；现已废除（详见 plan 精简 B）。
//
// 【返回值】(await_resume)
//   > 0：本次收到的字节数（已 hasWritten 到 buf 末尾）
//   = 0：对端正常关闭（FIN）
//   < 0：-errno（POSIX 原始错误码，不再有 -ENOBUFS）
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/buffer.hpp"
#include "coro_net/io/io_uring.h"

#include <liburing.h>
#include <sys/types.h>

namespace coro_net {

class RecvIntoBufferAwaiter : public IoOperationBase {
public:
    // 每次 recv 至少预留这么多 writable 字节
    static constexpr size_t kMinRecvSize = 4096;

    RecvIntoBufferAwaiter(int fd, Buffer& buf,
                          Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(&buf) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        buf_->ensureWritableBytes(kMinRecvSize);
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        io_uring_prep_recv(sqe, fd_, buf_->beginWrite(), buf_->writableBytes(), 0);
        prepare_common(sqe, h);
    }

    void on_complete(int32_t res, uint32_t flags) noexcept override {
        result_ = res;
        cqe_flags_ = flags;
        if (res > 0) {
            buf_->hasWritten(static_cast<size_t>(res));
        }
        if (handle_ && !handle_.done()) {
            sched_->push_ready(handle_);
        }
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int fd_;
    Buffer* buf_;
};

}  // namespace coro_net
