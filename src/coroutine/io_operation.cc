// =============================================================================
// io_operation.cc — IoOperationBase 公共方法实现
// =============================================================================
//
// 本文件只放需要 include <liburing.h> 的实现细节，把它隔离在 .cc 里，
// 让对外的 io_operation.hpp 保持轻量（只前置声明 io_uring_sqe）。
// =============================================================================

#include "coro_net/io_operation.hpp"
#include "coro_net/scheduler.hpp"

#include <liburing.h>

namespace coro_net {

// -----------------------------------------------------------------------------
// 把 awaiter 的地址塞进 SQE 的 user_data 字段，并记录协程 handle。
//
// 【关键不变量】
//   - 调用 prepare_common 之前，派生类必须已经 io_uring_prep_xxx(sqe, ...)
//     填好 opcode、fd、buf 等所有 op 专属字段。
//   - 调用之后，派生类 await_suspend 必须 *立刻 return*（即不能再继续访问
//     awaiter 字段做修改）；因为协程一旦挂起，scheduler 可能立刻看到 CQE
//     并把 result_ 写进来。
//
// 【user_data 编码】
//   现阶段直接放裸指针。后续若引入 op_type 高位标记（用来在日志中区分 op），
//   会改为：
//     uintptr_t data = reinterpret_cast<uintptr_t>(this) | (op_type << 56);
//   注意：x86_64 用户态地址只用低 47 位，所以高 8 位空闲可用。
// -----------------------------------------------------------------------------
void IoOperationBase::prepare_common(io_uring_sqe* sqe,
                                     std::coroutine_handle<> h) noexcept {
    handle_ = h;
    io_uring_sqe_set_data(sqe, this);
}

// -----------------------------------------------------------------------------
// on_complete 默认实现：single-shot awaiter 用
//   1) 存结果
//   2) 把 handle_ 推到 sched_->ready_，事件循环下一轮 resume
// -----------------------------------------------------------------------------
void IoOperationBase::on_complete(int32_t res, uint32_t flags) noexcept {
    result_ = res;
    cqe_flags_ = flags;
    if (handle_ && !handle_.done()) {
        sched_->push_ready(handle_);
    }
}

}  // namespace coro_net
