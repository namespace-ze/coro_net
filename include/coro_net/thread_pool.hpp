// =============================================================================
// coro_net/thread_pool.hpp —— 业务线程池，与协程协同
// =============================================================================
//
// 【为什么需要业务线程池】
//
//   io_uring 让 IO 不阻塞 worker 线程，但*CPU-bound* 任务（例如 protobuf 反射、
//   加解密、压缩、磁盘文件解析）仍然会"占着茅坑"——如果在 IO worker 线程上
//   跑 50ms 的业务计算，期间该 worker 上所有连接的 IO 都被串行化。
//
//   解决：把 CPU 任务投递到独立的"业务线程池"运行，让 IO worker 同时继续处理
//   其它连接的 IO。业务线程跑完后通过协程恢复机制把结果送回原 IO worker。
//
// 【协程视角的用法】
//
//   coro_net::CoroThreadPool pool("WorkerPool", 4);
//   pool.start();
//
//   Task<void> handler(TcpConnectionPtr conn) {
//       Buffer buf;
//       while (true) {
//           co_await conn->recv(buf);
//           auto request = parse(buf);
//
//           // ↓ 业务计算在 pool 里跑，handler 协程挂起
//           std::string response = co_await pool.submit([request]{
//               return service.CallMethod(request);   // 同步 CPU 工作
//           });
//
//           // ↓ 协程在原 IO worker 上 resume（fd 没有跨线程）
//           co_await conn->send(response);
//       }
//   }
//
// 【关键时序】
//
//   IO worker (handler 协程)              CoroThreadPool worker
//   ------------------------              ---------------------
//   co_await pool.submit(fn)
//      ↓
//   await_suspend:
//     - 记下当前 Scheduler* (src)
//     - 记下 coroutine_handle (h)
//     - 把 task 推到 pool 队列
//     - 协程挂起
//                                          被 cv 唤醒
//                                          取出 task
//                                          运行 fn() → 结果存到 awaiter
//                                          调用 src->post(h)
//                                            └→ eventfd_write 唤醒 src
//   src ring 醒来
//   drain_cross_queue 拿到 h
//   resume(h)
//      ↓
//   await_resume 拿结果返回
//
// 【线程安全】
//   - 队列：std::mutex + std::condition_variable
//   - awaiter 字段：suspend 期间被业务线程独占写、resume 后被协程独占读，
//     借由 happens-before（mutex unlock + cv notify 与 wait）保证可见性
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/scheduler.hpp"

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace coro_net {

class CoroThreadPool {
public:
    CoroThreadPool(std::string name, size_t threads);
    ~CoroThreadPool();

    CoroThreadPool(const CoroThreadPool&) = delete;
    CoroThreadPool& operator=(const CoroThreadPool&) = delete;

    void start();
    void stop();   // 通知所有 worker 退出并 join

    // 业务线程数
    size_t size() const noexcept { return threads_.size(); }

    // -------------------------------------------------------------------------
    // submit —— 提交一个 CPU 任务，返回一个 awaitable
    // -------------------------------------------------------------------------
    // 用法：
    //   auto x = co_await pool.submit([] { return heavy_compute(); });
    //
    // 模板细节：
    //   F: 可调用对象类型（函数 / lambda / std::function）
    //   R = invoke_result_t<F>
    //   返回的对象是 SubmitAwaiter<R, F>，它直接实现 Awaitable 三件套，
    //   无需再多一层 Task<R> 包装。
    // -------------------------------------------------------------------------
    template <typename F>
    auto submit(F&& fn);

    // 内部入队接口（被 awaiter 调用）；用 std::function<void()> 是因为
    // 我们要把 *awaiter 的 this 指针* 捕获进去，类型擦除后存到普通队列里。
    void enqueue(std::function<void()> task);

private:
    void worker_loop();

    std::string name_;
    size_t target_threads_;
    std::vector<std::thread> threads_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> queue_;
    std::atomic_bool stopping_{false};
    bool started_ = false;
};

// -----------------------------------------------------------------------------
// SubmitAwaiter —— pool.submit 返回的类型
// -----------------------------------------------------------------------------
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
    void await_suspend(std::coroutine_handle<> h) noexcept {
        handle_ = h;
        src_sched_ = Scheduler::current();
        // 把"运行 + 写结果 + 唤醒"作为一个 lambda 投递
        pool_->enqueue([this]() {
            try {
                if constexpr (std::is_void_v<R>) {
                    fn_();
                    result_.template emplace<1>(std::monostate{});
                } else {
                    result_.template emplace<1>(fn_());
                }
            } catch (...) {
                result_.template emplace<2>(std::current_exception());
            }
            // 业务线程完成 → 通过 eventfd 兜底路径让 src_sched 唤醒
            // 并把 handle 送回 ready 队列（drain_cross_queue 完成）
            src_sched_->post(handle_);
        });
    }

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

template <typename F>
auto CoroThreadPool::submit(F&& fn) {
    using R = std::invoke_result_t<std::decay_t<F>>;
    using FT = std::decay_t<F>;
    return SubmitAwaiter<R, FT>(this, std::forward<F>(fn));
}

}  // namespace coro_net
