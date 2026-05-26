// =============================================================================
// coro_net/fire_and_forget.hpp —— 顶层"扔出去就不管"的协程入口
// =============================================================================
//
// 【用途】Scheduler 启动一个 handler 协程，handler 跑完自动销毁帧，
//        没有人 await 它的结果。例如：
//          scheduler.spawn([&]() -> FireAndForget {
//              co_await handle_connection(conn);
//          }());
//
// 【与 Task 的区别】
//    Task: lazy，必须有人 co_await 才会跑；结束后由所有者销毁
//    FireAndForget: eager，立刻执行；结束后自己销毁帧
// =============================================================================

#pragma once

#include <coroutine>
#include <exception>

namespace coro_net {

namespace detail {
// 接入 Logger 的非内联实现，避免本头文件 include log.hpp 引入沉重依赖
[[noreturn]] void log_fire_and_forget_uncaught() noexcept;
}  // namespace detail

struct FireAndForget {
    struct promise_type {
        FireAndForget get_return_object() noexcept { return {}; }
        // 立刻执行：initial 不挂起
        std::suspend_never initial_suspend() noexcept { return {}; }
        // 跑完自动销毁帧：final 不挂起 → 控制权回到 resume 调用者，
        //               协程帧被编译器生成的代码自动 delete
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            // FireAndForget 顶层捕获异常，没人能 rethrow。
            // 通过 detail::log_fire_and_forget_uncaught 写一行 LOG_FATAL 再 abort。
            detail::log_fire_and_forget_uncaught();
        }
    };
};

}  // namespace coro_net
