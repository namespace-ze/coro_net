// =============================================================================
// coro_net/io_operation.hpp — io_uring 操作 awaiter 的公共基类
// =============================================================================
//
// 【作用】
//   IoOperationBase 是所有具体 io_uring 操作 awaiter 的共同父类：
//     - RecvAwaiter / SendAwaiter / AcceptAwaiter / TimeoutAwaiter / ShutdownAwaiter
//   它统一管理：
//     1) 结果字段 result_、cqe_flags_：CQE 回来时由 Scheduler 写入
//     2) 协程 handle_：CQE 回来时由 Scheduler 调用 resume()
//
// 【为什么不用 virtual】
//   C++20 awaitable 的 await_ready/suspend/resume 是 *按静态类型* 解析的，
//   所以没办法让基类提供一个虚函数让派生类填 SQE。
//   解决方案：派生类自己写 await_suspend，里面做完 "填 SQE + set_data(this)" 之后
//   挂起；具体的 fill_sqe 逻辑由派生类直接内联实现，避免 vtable。
//
// 【用户视角看一次 IO 的生命周期】（以 recv 为例）
//
//   1) 用户代码：   ssize_t n = co_await conn.recv(buf);
//                              ^^^^^^^^^^^^^^^^^^^^^^^^
//                          构造一个临时 RecvAwaiter（IoOperationBase 派生类），
//                          这个临时变量被绑定在 *协程帧* 里，挂起期间一直存活
//
//   2) 编译器调用 awaiter.await_ready()  → 返回 false → 准备挂起
//
//   3) 编译器调用 awaiter.await_suspend(coroutine_handle)：
//        - 把 coroutine_handle 存到 this->handle_
//        - 从本线程的 io_uring 取一个 SQE
//        - io_uring_prep_recv(sqe, fd, buf, len, 0)
//        - io_uring_sqe_set_data(sqe, this)     ★ user_data = 自己的 this 指针
//        - 协程挂起，控制权返回 Scheduler 事件循环
//
//   4) 内核把 IO 提交给网络栈，IO 完成后写入 CQE 到本线程的 CQ
//
//   5) Scheduler::run() 在 wait_cqe 处醒来，遍历所有 CQE：
//        - 从 cqe->user_data 还原 IoOperationBase* (即原 awaiter)
//        - 调用 op->complete(cqe->res, cqe->flags)
//        - 把 op->handle_ 入队到 ready_，下一轮 resume
//
//   6) handle_.resume() 触发 await_resume()：派生类返回 result_ 给用户协程
//
// 【user_data 的编码约定】
//   io_uring 的 user_data 是 uint64_t，我们直接放裸指针 IoOperationBase*。
//   但为了调试方便（CQE 风暴时定位发送方），我们*可以*在高 8 位编码 op_type。
//   现阶段（S1 骨架）先用纯指针，S3 阶段拼出完整 awaiter 时再加 op_type 标记。
//
// 【为什么 handle_ 必须存到 awaiter 而不是别处】
//   await_suspend(h) 是编译器给我们交接 h 的唯一时机。挂起以后，编译器把整个
//   awaiter 留在协程帧上，所以 awaiter 的字段在挂起期间是稳定可达的。
//   Scheduler 从 cqe->user_data 拿到 awaiter 指针后，就能取出 handle_ 并 resume。
//
// 【对照 mymuduo Channel】
//   mymuduo: Channel 拥有 fd + read/write/close callback；Poller 把就绪的 channel
//            塞回 EventLoop 的 activeChannels_，EventLoop 调用 channel->handleEvent。
//   coro_net: 每个 IO 操作有自己独立的 awaiter（一次性对象），生命周期就是一次
//             co_await；不需要长期持有 fd 与一组 callback。表达力更强、内存更轻。
// =============================================================================

#pragma once

#include <coroutine>
#include <cstdint>

struct io_uring_sqe;  // 前置声明，避免在头文件 include liburing.h

namespace coro_net {

class Scheduler;  // S3 阶段实现，本头文件只用前置声明

// -----------------------------------------------------------------------------
// IoOperationBase
// -----------------------------------------------------------------------------
//
// 【字段说明】
//   sched_      ：本次 IO 关联的 Scheduler，决定 SQE 提交到哪个 io_uring；
//                 由派生类构造时传入。本库模型 A 下，sched_ = 协程当前所在 worker
//                 的 Scheduler（保证 fd 不跨线程迁移）。
//   handle_     ：协程 handle，由 await_suspend 设置；CQE 回来后被 Scheduler 用
//                 来恢复协程执行。
//   result_     ：CQE 的 res 字段：>0 是字节数，<0 是 -errno。语义同 syscall 返回值。
//   cqe_flags_  ：CQE 的 flags 字段：multishot 是否继续 (IORING_CQE_F_MORE)、
//                 是否携带 provide-buffer ID (IORING_CQE_F_BUFFER) 等。
// -----------------------------------------------------------------------------
class IoOperationBase {
public:
    explicit IoOperationBase(Scheduler& sched) noexcept : sched_(&sched) {}

    virtual ~IoOperationBase() = default;

    // 禁拷贝禁移动：awaiter 是临时对象，地址作为 user_data 注入内核，
    // 一旦 SQE 提交了，地址就不能变。
    IoOperationBase(const IoOperationBase&) = delete;
    IoOperationBase& operator=(const IoOperationBase&) = delete;
    IoOperationBase(IoOperationBase&&) = delete;
    IoOperationBase& operator=(IoOperationBase&&) = delete;

    // -------------------------------------------------------------------------
    // on_complete —— CQE 到达时由 Scheduler 调用
    // -------------------------------------------------------------------------
    // 【默认行为】（single-shot awaiter 用）：
    //   1. 把 res / flags 存进 awaiter
    //   2. 把 handle_ 入 ready 队列，下一轮 resume 协程
    //
    // 【派生类可以重写】（multishot op 用）：
    //   不 resume 协程（没有协程），而是把每次 CQE 当成一个事件回调，
    //   持续 alive 直到 IORING_CQE_F_MORE 未设置（multishot 终止）。
    //
    // 【为什么用 virtual】
    //   awaitable 的 await_ready/suspend/resume 由编译器按静态类型查找，
    //   不能 virtual；但 Scheduler 从 user_data 拿到的是 IoOperationBase*，
    //   需要派发到具体子类的 CQE 处理逻辑，所以这里 virtual 是必须的。
    //   single-shot awaiter 用默认实现，开销忽略不计（每次 IO 一次虚调用）。
    // -------------------------------------------------------------------------
    virtual void on_complete(int32_t res, uint32_t flags) noexcept;

    // 旧的 complete 接口：仅写字段，不入队（兼容已有 awaiter）
    void complete(int32_t res, uint32_t flags) noexcept {
        result_ = res;
        cqe_flags_ = flags;
    }

    std::coroutine_handle<> handle() const noexcept { return handle_; }

    // -------------------------------------------------------------------------
    // Awaitable 三件套的默认实现
    // -------------------------------------------------------------------------
    // 派生类一般直接用默认 await_ready/await_resume，只重写 await_suspend
    // 来调用 io_uring_prep_xxx 函数（即不同 op 的区别仅在 fill_sqe）。
    // -------------------------------------------------------------------------
    bool await_ready() const noexcept { return false; }
    int32_t await_resume() noexcept { return result_; }

    // 子类辅助：填 SQE 公共部分（设置 user_data 为 this，记录 handle_）。
    // 派生类应在 await_suspend 中：
    //     auto* sqe = sched_->ring().get_sqe();
    //     io_uring_prep_recv(sqe, ...);   // 各自不同
    //     prepare_common(sqe, h);          // 公共部分
    void prepare_common(io_uring_sqe* sqe, std::coroutine_handle<> h) noexcept;

protected:
    Scheduler* sched_;
    std::coroutine_handle<> handle_{};
    int32_t result_ = 0;
    uint32_t cqe_flags_ = 0;
};

}  // namespace coro_net
