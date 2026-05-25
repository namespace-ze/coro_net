// =============================================================================
// coro_net/ops.hpp — 五种 io_uring 操作的 awaiter
// =============================================================================
//
// 提供以下 5 个 awaiter（继承自 IoOperationBase）：
//   AcceptAwaiter   ：accept 新连接（single-shot；multishot 版本 S4 引入）
//   RecvAwaiter     ：从 socket 读取（先 single-shot 版；multishot + buffer-ring
//                     版作为 RecvMultishot 单独提供）
//   SendAwaiter     ：向 socket 写入
//   TimeoutAwaiter  ：等待一段时间（OP_TIMEOUT，比 sleep 精确）
//   ShutdownAwaiter ：半关闭连接（OP_SHUTDOWN）
//
// 【共同模式】
//   每个 awaiter 都是：
//     1. 构造时记下参数（fd、buf、ts 等）
//     2. await_suspend(h)：
//          - 从 Scheduler::current()->ring() 取 SQE
//          - io_uring_prep_xxx 填字段
//          - IoOperationBase::prepare_common(sqe, h) 把 this 写进 user_data
//          - 立刻 return（不需要主动 submit；run() 在 wait_cqe 前会 submit）
//     3. CQE 回来时：Scheduler 从 user_data 还原 this，调用 complete()
//                   并把 handle 入 ready 队列
//     4. handle.resume() 触发 await_resume()：派生类返回结果（result_ 或派生字段）
// =============================================================================

#pragma once

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/buffer.hpp"
#include "coro_net/io/io_uring.h"
#include "coro_net/io/buffer_ring.h"

#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <chrono>
#include <cstring>

namespace coro_net {

// -----------------------------------------------------------------------------
// AcceptAwaiter —— 接受一个新连接（single-shot）
// -----------------------------------------------------------------------------
// 用法：
//     int conn_fd = co_await AcceptAwaiter{listen_fd};
//     if (conn_fd < 0) { ... }
//
// 返回值（await_resume）：
//     >= 0 ：新的连接 fd
//     <  0 ：-errno
// -----------------------------------------------------------------------------
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

    // 取连接 fd 的同时，调用者也可以查询 peer 地址：
    int await_resume() noexcept { return result_; }

    const sockaddr_in& peer() const noexcept { return peer_addr_; }
    socklen_t peer_len() const noexcept { return peer_len_; }

private:
    int listen_fd_;
    sockaddr_in peer_addr_{};
    socklen_t peer_len_{};
};

// -----------------------------------------------------------------------------
// RecvAwaiter —— single-shot 普通 recv
// -----------------------------------------------------------------------------
// 用法：
//     char buf[4096];
//     ssize_t n = co_await RecvAwaiter{fd, buf, sizeof(buf)};
//     n > 0 ：实际读到的字节
//     n == 0：对端正常关闭
//     n < 0 ：-errno
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// RecvIntoBufferAwaiter —— 用 provide-buffers 收数据并 memcpy 到 per-conn Buffer
// -----------------------------------------------------------------------------
//
// 【设计目标】综合 provide-buffers 节省内存 + per-conn Buffer 连续语义
//
//   不指定 recv 缓冲区，让内核从本 worker 的 BufferRing 挑一个槽位写入；
//   CQE 回来后把数据 memcpy 到调用者的 Buffer，归还槽位。
//
// 【为什么这样设计】（详见 plan §Buffer 与 provide-buffers 协作）
//   - Buffer 内部数据连续，方便协议拆帧 peek/retrieve
//   - BufferRing 内存占用 O(M)，与连接数 N 无关
//   - 单次 memcpy ~200ns，远小于 syscall 节省的开销
//
// 【返回值】(await_resume)
//   > 0：本次收到的字节数（已 append 到 buf 末尾）
//   = 0：对端正常关闭（FIN）
//   < 0：-errno（如 -ENOBUFS：buffer ring 暂时无可用槽位 → 应延后重试）
// -----------------------------------------------------------------------------
class RecvIntoBufferAwaiter : public IoOperationBase {
public:
    RecvIntoBufferAwaiter(int fd, Buffer& buf,
                          Scheduler& s = *Scheduler::current())
        : IoOperationBase(s), fd_(fd), buf_(&buf) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        // 不传 addr / len，让内核从 buffer ring 挑
        io_uring_prep_recv(sqe, fd_, nullptr, 0, 0);
        sqe->flags |= IOSQE_BUFFER_SELECT;
        sqe->buf_group = sched_->buffer_ring().group_id();
        prepare_common(sqe, h);
    }

    // 重写 on_complete：把 ring buffer 中的数据 memcpy 到 Buffer 并归还
    void on_complete(int32_t res, uint32_t flags) noexcept override {
        result_ = res;
        cqe_flags_ = flags;
        if (res > 0 && (flags & IORING_CQE_F_BUFFER)) {
            uint16_t bid = flags >> IORING_CQE_BUFFER_SHIFT;
            auto view = sched_->buffer_ring().view(bid);
            buf_->append(view.data(), (size_t)res);
            sched_->buffer_ring().return_buffer(bid);
        }
        // 没有 F_MORE（single-shot）→ resume 协程
        if (handle_ && !handle_.done()) {
            sched_->push_ready(handle_);
        }
    }

    ssize_t await_resume() noexcept { return result_; }

private:
    int fd_;
    Buffer* buf_;
};

// -----------------------------------------------------------------------------
// SendAwaiter —— single-shot 普通 send
// -----------------------------------------------------------------------------
// 注意：一次 send 不保证发完所有字节。生产代码应在外层循环里检查 result_
// 并对 buf+result_、len-result_ 继续 send。本 awaiter 不做循环（保持简单）。
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// TimeoutAwaiter —— 等待一段时间
// -----------------------------------------------------------------------------
// 用法：
//     co_await TimeoutAwaiter{std::chrono::milliseconds(500)};
//
// 实现：用 IORING_OP_TIMEOUT，参数是 __kernel_timespec。
// 注意：timespec 必须在 IO 完成前一直有效——放在 awaiter 成员里即可。
// -----------------------------------------------------------------------------
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

// -----------------------------------------------------------------------------
// ShutdownAwaiter —— 半关闭连接
// -----------------------------------------------------------------------------
// 用法：
//     co_await ShutdownAwaiter{fd, SHUT_WR};
//     close(fd);            // 半关闭后通常配合 close
//
// SHUT_RD：禁止接收
// SHUT_WR：禁止发送（发送 FIN，对端 read 返回 0）
// SHUT_RDWR：双向关闭
// -----------------------------------------------------------------------------
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
