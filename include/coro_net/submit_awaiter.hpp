// =============================================================================
// coro_net/submit_awaiter.hpp —— CoroThreadPool::submit 返回的 awaiter
// =============================================================================
// CoroThreadPool::submit() 的定义见 thread_pool.hpp 末尾。
// =============================================================================

#pragma once

#include "coro_net/scheduler.hpp"

#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro_net {

class CoroThreadPool;

template <typename R, typename F>
class SubmitAwaiter {
public:
    SubmitAwaiter(CoroThreadPool* pool, F fn)
        : pool_(pool), fn_(std::move(fn)) {}

    SubmitAwaiter(const SubmitAwaiter&) = delete;
    SubmitAwaiter& operator=(const SubmitAwaiter&) = delete;
    // 允许 move：临时对象需要被 co_await 表达式接管
    SubmitAwaiter(SubmitAwaiter&&) = default;

    bool await_ready() const noexcept { return false; }

    // -------------------------------------------------------------------------
    // await_suspend
    // -------------------------------------------------------------------------
    // 1) 记下"我在哪个 Scheduler 上挂起"——业务线程结束后要把 handle 送回这里
    // 2) 把"运行 fn 并通知"打包成 std::function<void()> 推到 pool 队列
    // 3) 返回（控制权交回 Scheduler，本协程已挂起）
    // -------------------------------------------------------------------------
    void await_suspend(std::coroutine_handle<> h) noexcept;

    R await_resume() {
        if (result_.index() == 2) {
            std::rethrow_exception(std::get<2>(result_));
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(std::get<1>(result_));
        }
    }

private:
    // result_ index: 0=未完成, 1=值/void, 2=异常
    using Slot = std::conditional_t<std::is_void_v<R>, std::monostate, R>;
    std::variant<std::monostate, Slot, std::exception_ptr> result_;
    CoroThreadPool* pool_;
    F fn_;
    std::coroutine_handle<> handle_{};
    Scheduler* src_sched_ = nullptr;
};

}  // namespace coro_net
