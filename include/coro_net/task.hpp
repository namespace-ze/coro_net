// =============================================================================
// coro_net/task.hpp — 协程返回类型 Task<T>
// =============================================================================
// FireAndForget 已拆分至 coro_net/fire_and_forget.hpp。
//
// 【背景知识：C++20 协程三大关键字】
//   - co_await expr  ：挂起当前协程，等待 expr（必须是 Awaitable）；恢复后取值
//   - co_yield val   ：生成器模式专用（本库暂未使用，留给 AsyncGenerator）
//   - co_return val  ：协程结束并返回值（类似普通函数的 return）
//
// 【协程函数的本质】
//   编译器看到函数体内有上述任一关键字，就把这个函数变成一个状态机：
//     1. 调用该函数时，编译器先在堆上 new 一个"协程帧"（coroutine frame），
//        里面保存了局部变量、当前执行到的状态点、promise_type 实例
//     2. 函数立即返回一个由 promise_type::get_return_object() 构造的对象
//        ——本库中这个对象就是 Task<T>
//     3. 真正的执行交给 std::coroutine_handle<promise_type> 这个把手
//        通过 handle.resume() / handle.destroy() 来推进协程或销毁它
//
// 【Task<T> 设计选择：lazy】
//   initial_suspend 返回 std::suspend_always，表示协程一创建就挂起，
//   不会立刻执行函数体。直到外层 co_await task 才真正启动它。
//   优势：
//     - 对外语义清晰：Task<T> 是"承诺"，co_await 才"兑现"
//     - 结合 symmetric transfer（见 final_suspend 注释），嵌套 co_await 不会爆栈
//   类比 mymuduo：相当于把"立即执行的 std::function<void()> 回调"
//                改成"延迟启动、可链式 await 的任务"
//
// 【FireAndForget 设计选择：eager + self-destroy】
//   顶层入口（例如 Scheduler::spawn 启动一个 handler 协程）需要"扔出去就不管"，
//   所以 initial_suspend 返回 std::suspend_never（立刻执行），
//   final_suspend 也返回 std::suspend_never（协程结束自动销毁帧）。
//
// 【与 mymuduo 回调风格的对照】
//   mymuduo:
//     server.setMessageCallback([](TcpConnectionPtr conn, Buffer* buf, Timestamp){
//         auto data = buf->retrieveAllAsString();
//         conn->send(process(data));  // 同步调用，业务在 sub-reactor 上跑
//     });
//
//   coro_net:
//     Task<void> handler(TcpConnectionPtr conn) {
//         Buffer buf;
//         while (true) {
//             co_await conn->recv(buf);            // 异步等数据，不阻塞线程
//             auto data = buf.retrieveAllAsString();
//             auto rsp = co_await pool.submit([&]{ // 业务跑到独立线程池
//                 return process(data);
//             });
//             co_await conn->send(rsp);            // 异步发响应
//         }
//     }
//   两者业务逻辑一致，但协程版本读起来像同步代码，省去了回调嵌套。
// =============================================================================

#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

namespace coro_net {

// -----------------------------------------------------------------------------
// 前置声明
// -----------------------------------------------------------------------------
template <typename T>
class Task;

// -----------------------------------------------------------------------------
// FinalAwaiter —— Task 的 final_suspend 返回值
// -----------------------------------------------------------------------------
//
// 【作用】协程函数体执行完 co_return 之后，编译器会自动 co_await final_suspend()。
//        我们让 final_suspend 返回这个 awaiter，从而在协程结束时"对称转移"控制权
//        给那个在 co_await 我的外层协程。
//
// 【为什么需要 symmetric transfer】
//    考虑这个调用链：
//        A 协程 co_await B  → B 协程 co_await C → C 完成
//    如果 final_suspend 不做 symmetric transfer，而是用 handle.resume() 恢复外层，
//    那么 C 恢复 B 是函数调用（栈深 +1），B 恢复 A 又是函数调用（栈深 +2），
//    嵌套 N 层就是 N 层栈，可能爆栈。
//
//    symmetric transfer 把 "C 完成 → 跳到 B 继续执行" 变成 *尾调用*：
//    C 的栈帧已经被销毁，B 直接拿到 CPU 控制权，栈深保持常量。
//
//    实现上：await_suspend 不返回 void / bool，而是返回 std::coroutine_handle<>，
//    编译器看到这个签名，会把"跳转到这个 handle"翻译成 jmp 而非 call。
// -----------------------------------------------------------------------------
struct FinalAwaiter {
    // await_ready 返回 false 表示"我永远需要挂起一下"——这样编译器才会调用
    // await_suspend 让我们有机会做 symmetric transfer。
    bool await_ready() const noexcept { return false; }

    // 【关键】返回 coroutine_handle<> 触发 symmetric transfer。
    // 参数 h 是"刚刚完成的协程的 handle"（即 Task 自己），用不到。
    // 我们要跳转到的目标是 promise_type::continuation，
    // 也就是"那个正在 co_await 等我的外层协程"。
    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        auto& promise = h.promise();
        // 如果有人在等我，就跳过去执行它；
        // 如果没有人等（顶层 Task 没被 co_await 过），那么 continuation 是
        // noop_coroutine —— jmp 到 noop_coroutine 等于"什么都不做、回到事件循环"。
        return promise.continuation_;
    }

    void await_resume() noexcept {}
};

// -----------------------------------------------------------------------------
// TaskPromise —— Task<T> 的 promise_type
// -----------------------------------------------------------------------------
//
// 【promise_type 的角色】
//    编译器要求每个协程函数都有一个关联的 promise_type，它负责：
//      1. 创建返回值对象（get_return_object）
//      2. 控制初始挂起策略（initial_suspend）
//      3. 控制结束挂起策略（final_suspend）
//      4. 接收 co_return 的值（return_value / return_void）
//      5. 处理未捕获异常（unhandled_exception）
//
// 【数据成员】
//    result_       ：保存 co_return 的值 或 抛出的异常
//    continuation_ ：保存"在等我的外层协程"的 handle，
//                   final_suspend 通过它实现 symmetric transfer
// -----------------------------------------------------------------------------
template <typename T>
struct TaskPromise {
    // 结果存储：未完成时是 monostate；正常完成是 T；异常完成是 exception_ptr。
    // 用 variant 是为了同时支持值与异常两种"完成态"。
    std::variant<std::monostate, T, std::exception_ptr> result_;
    std::coroutine_handle<> continuation_ = std::noop_coroutine();

    // 1. 创建外层可见的 Task 对象
    Task<T> get_return_object() noexcept;

    // 2. 初始挂起：lazy（创建后不执行，等待被 co_await 时再启动）
    std::suspend_always initial_suspend() noexcept { return {}; }

    // 3. 结束挂起：返回 FinalAwaiter 触发 symmetric transfer
    FinalAwaiter final_suspend() noexcept { return {}; }

    // 4. co_return val 会调用这里，把值存进 variant
    template <typename U>
    void return_value(U&& v) {
        result_.template emplace<1>(std::forward<U>(v));
    }

    // 5. 协程内抛出未捕获异常时调用，把异常打包到 result_，
    //    在外层 await_resume 中重新抛出。
    void unhandled_exception() noexcept {
        result_.template emplace<2>(std::current_exception());
    }
};

// void 偏特化：return_void 而非 return_value
template <>
struct TaskPromise<void> {
    // 用 variant 表示三种状态：monostate=未完成, monostate=已完成(void), exception_ptr
    // 为了避免歧义，我们用 bool 标记是否完成
    std::exception_ptr exception_ = nullptr;
    std::coroutine_handle<> continuation_ = std::noop_coroutine();

    Task<void> get_return_object() noexcept;

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
};

// -----------------------------------------------------------------------------
// Task<T> —— 协程返回类型
// -----------------------------------------------------------------------------
//
// 【生命周期】
//    Task 持有 coroutine_handle 的所有权。析构时若 handle 未被 move 走，
//    则销毁协程帧（避免泄漏）。
//
// 【典型用法】
//    Task<int> compute() { co_return 42; }
//
//    Task<void> caller() {
//        int x = co_await compute();   // 这里 compute() 启动 + 跑完 + 拿到 42
//        // x == 42
//    }
//
// 【线程上下文】
//    Task 本身不绑定线程；它在哪个线程被 resume，回调代码就在哪个线程跑。
//    本库 Scheduler 模型 A 保证：一个 handler 协程从启动到结束都在同一 worker
//    线程上 resume，所以协程帧内的局部变量天然单线程访问，**不需要加锁**。
// -----------------------------------------------------------------------------
template <typename T>
class [[nodiscard]] Task {
public:
    using promise_type = TaskPromise<T>;
    using Handle = std::coroutine_handle<promise_type>;

    // 构造：把协程 handle 挪进来
    explicit Task(Handle h) noexcept : handle_(h) {}

    // 禁止拷贝（一个 handle 只能有一个所有者）
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // 允许 move
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    // 析构：销毁协程帧（lazy task 创建后若没被 co_await 也要回收）
    ~Task() {
        if (handle_) handle_.destroy();
    }

    // -------------------------------------------------------------------------
    // Awaitable 接口：让 Task 自己也能被 co_await
    // -------------------------------------------------------------------------
    //
    // 三段式语义（C++20 标准定义）：
    //   1. await_ready: 询问"是否可以直接拿结果，免去挂起"。
    //                  对 lazy Task 而言永远 false（必须先启动）。
    //   2. await_suspend: 已决定挂起，编译器把外层协程 handle 给我们。
    //                    我们要做的是：
    //                      (a) 把外层的 handle 记到 self.promise().continuation_，
    //                          这样 self 跑完后 final_suspend 知道跳回哪里
    //                      (b) 返回 self.handle()，触发 symmetric transfer，
    //                          直接跳到 self 的入口开始执行
    //   3. await_resume: self 跑完后从 final_suspend 跳回，编译器调用这个取结果。
    //                   如果 result_ 里是异常，则重新抛出。
    // -------------------------------------------------------------------------

    bool await_ready() const noexcept {
        // handle 为空（不应发生）或已经 done 则不挂起。
        // 但 lazy task 创建后还没 resume 过，handle.done() 是 false。
        return !handle_ || handle_.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        // 记录"是谁在等我"
        handle_.promise().continuation_ = caller;
        // 返回 self.handle() → 编译器生成 jmp 指令直接跳到 self 入口，
        // 而 caller 的栈帧已被挂起（保存到协程帧里），栈深不增长。
        return handle_;
    }

    T await_resume() {
        // self 已经 co_return 或抛异常，从 result_ 取值
        auto& result = handle_.promise().result_;
        if (result.index() == 2) {
            // 异常分支：把当时存的 exception_ptr 重新抛出
            std::rethrow_exception(std::get<2>(result));
        }
        // 正常分支
        return std::move(std::get<1>(result));
    }

    // 返回内部 handle（供 Scheduler::spawn 等内部用，用户一般不用）
    Handle handle() const noexcept { return handle_; }

    // 把 handle 所有权移交出去（之后本对象不再 destroy 它）
    Handle release() noexcept { return std::exchange(handle_, {}); }

private:
    Handle handle_;
};

// void 偏特化（实现与上面几乎一样，区别在 await_resume 不返回值）
template <>
class [[nodiscard]] Task<void> {
public:
    using promise_type = TaskPromise<void>;
    using Handle = std::coroutine_handle<promise_type>;

    explicit Task(Handle h) noexcept : handle_(h) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Task() { if (handle_) handle_.destroy(); }

    bool await_ready() const noexcept { return !handle_ || handle_.done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        handle_.promise().continuation_ = caller;
        return handle_;
    }

    void await_resume() {
        if (handle_.promise().exception_) {
            std::rethrow_exception(handle_.promise().exception_);
        }
    }

    Handle handle() const noexcept { return handle_; }
    Handle release() noexcept { return std::exchange(handle_, {}); }

private:
    Handle handle_;
};

// promise_type::get_return_object 的定义放在 Task 定义之后
template <typename T>
inline Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

}  // namespace coro_net
