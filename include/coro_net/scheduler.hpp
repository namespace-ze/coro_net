// =============================================================================
// coro_net/scheduler.hpp — 单 IO worker 的调度器
// =============================================================================
//
// 【角色定位】
//
//   每个 IO worker 线程持有一个 Scheduler 实例。Scheduler 是这个线程的"大脑"：
//     - 拥有一个 io_uring 实例（SQ/CQ 不与其他线程共享，完全无锁）
//     - 拥有一个 TimerQueue（per-worker 计时器堆）
//     - 拥有一个 ready 队列：当前可立即 resume 的协程
//     - 跑事件循环 run()：交替处理 ready 队列与 CQE
//
// 【事件循环算法】
//
//   while (!stopping_) {
//       // 1) 处理本线程内同步产生的 ready 协程（spawn 出来还没跑过的、
//       //    被同步 wakeup 的、跨线程 post 进来的）
//       while (!ready_.empty()) {
//           handle = ready_.front(); ready_.pop_front();
//           handle.resume();         // 协程跑到下一个 co_await 或 co_return 后挂起
//       }
//       // 2) 提交本轮积累的所有 SQE
//       ring_.submit();
//
//       // 3) 阻塞等待至少一个 CQE
//       ring_.submit_and_wait(1);    // 注意：submit_and_wait 会再 submit 一次，
//                                    // 重复 submit 是 cheap 的，避免漏提交。
//       // 4) 批量取出 CQE 并 resume
//       io_uring_cqe* cqes[64];
//       unsigned n = ring_.peek_batch_cqe(cqes, 64);
//       for (i in 0..n) {
//           auto* op = (IoOperationBase*)io_uring_cqe_get_data(cqes[i]);
//           op->complete(cqes[i]->res, cqes[i]->flags);
//           if (!(cqes[i]->flags & IORING_CQE_F_MORE)) {
//               // 不是 multishot 持续 CQE，可以 resume 协程
//               ready_.push_back(op->handle());
//           } else {
//               // multishot 中间 CQE：op 自己处理（一般是把数据扔给消费者）
//               // 不 resume，因为 multishot awaiter 是生成器模式
//           }
//       }
//       ring_.cq_advance(n);
//   }
//
// 【thread_local current()】
//
//   awaiter 在 await_suspend 时需要知道"现在我在哪个 Scheduler 上"，
//   从而把 SQE 提交到对的 io_uring。但 awaiter 自己被构造时未必传入了 Scheduler*
//   （例如用户写 co_await sleep_for(1s)，没机会指定）。
//
//   解决方案：Scheduler::run() 在自己线程里把 this 存到 thread_local 槽位，
//   awaiter 通过 Scheduler::current() 拿到。
//
// 【对照 mymuduo::EventLoop】
//
//   mymuduo: EventLoop::loop() 调用 poller_->poll() 取就绪 channel，
//            遍历 channel->handleEvent() 派发回调；pendingFunctors_ 是跨线程
//            塞回 callback 的队列，eventfd 唤醒。
//   coro_net: Scheduler::run() 调用 io_uring submit_and_wait 取就绪 CQE，
//            从 user_data 还原 IoOperation 并恢复协程；ready_ 是本线程内
//            同步可恢复的协程队列；跨线程通过 MSG_RING（S4 引入）唤醒。
//
//   抽象映射：
//     EventLoop          ↔ Scheduler
//     Poller             ↔ IoUring
//     Channel + handleEvent ↔ IoOperationBase + complete + handle.resume()
//     pendingFunctors_   ↔ ready_ (本线程) + MSG_RING (跨线程)
//     eventfd 唤醒        ↔ MSG_RING 把 dummy CQE 塞进目标 ring
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/fire_and_forget.hpp"
#include "coro_net/timer/timer_id.hpp"
#include <coroutine>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>

namespace coro_net {

class IoUring;
class TimerQueue;
class RegisteredBufferPool;

// -----------------------------------------------------------------------------
// SchedulerConfig：构造 Scheduler 时的参数
// -----------------------------------------------------------------------------
struct SchedulerConfig {
    unsigned ring_entries = 8192;        // SQ/CQ 容量（高并发下需足够大，见 get_sqe 风险）

    // --- SQPOLL（默认关闭；由 TcpServer 按 set_sqpoll_threads 开启，单元测试不受影响）---
    bool     sqpoll = false;             // 启用 IORING_SETUP_SQPOLL
    unsigned sq_thread_idle_ms = 1000;   // 轮询线程空闲多久（ms）后休眠
    int      wq_fd = -1;                 // >=0：ATTACH_WQ 复用该 ring 的轮询线程
    int      sq_thread_cpu = -1;         // >=0：把本 ring 的 SQPOLL 线程钉到该核

    // --- 固定注册缓冲池（默认关闭；由 TcpServer 按 set_fixed_buffers 开启）---
    bool     use_fixed_buffers = false;  // 注册固定缓冲池（io_uring_register_buffers）
    unsigned buf_slot_size = 16 * 1024;  // 每 slot 字节
    unsigned buf_pool_capacity = 0;      // slot 数（0 = 不建池）
};

class Scheduler {
public:
    explicit Scheduler(SchedulerConfig cfg = {});
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // -------------------------------------------------------------------------
    // run —— 进入事件循环（阻塞当前线程，直到 stop 被调用）
    // -------------------------------------------------------------------------
    // 调用方：每个 IO worker 线程的入口函数。
    //
    // 进入时设置 thread_local current() = this，离开时清空。
    // -------------------------------------------------------------------------
    void run();

    // 通知事件循环退出：设置 stopping_ 标志，并提交一个 NOP SQE 唤醒
    // 可能正阻塞在 wait_cqe 的 run() 协程。
    void stop();

    // -------------------------------------------------------------------------
    // spawn —— 在本 Scheduler 上启动一个新协程
    // -------------------------------------------------------------------------
    // 必须在本 Scheduler 的线程内调用（即 current() == this）。
    // 对于 Task<void>：把它的 handle 入队，run() 下一轮处理。
    // 我们用一个壳子协程把 Task<void> "包装" 成 FireAndForget 风格
    // （没有人 await，跑完自动销毁帧）。
    // -------------------------------------------------------------------------
    void spawn(Task<void> task);

    // -------------------------------------------------------------------------
    // post —— 把一个 ready 协程塞到本 Scheduler 等待恢复
    // -------------------------------------------------------------------------
    // 调用线程：
    //   - 若是本 Scheduler 的线程：直接 push 到 ready 队列
    //   - 若是另一个 Scheduler 的线程：把 handle 放入 cross_.handles，
    //     然后用 io_uring_prep_msg_ring 把一个 dummy SQE 从本线程的 ring
    //     发到目标 ring，让目标 Scheduler 从 submit_and_wait 中唤醒。
    //   - 若调用线程没有 io_uring（如 CoroThreadPool 的 worker）：
    //     S6 阶段补充 eventfd 回退路径。当前 S4 阶段假定调用方在某个
    //     Scheduler 线程上。
    // -------------------------------------------------------------------------
    void post(std::coroutine_handle<> h);

    // -------------------------------------------------------------------------
    // post_task —— 把一段普通代码扔到本 Scheduler 线程执行
    // -------------------------------------------------------------------------
    // 用途：Acceptor 拿到新 fd 后，要在目标 worker 线程上"创建 TcpConnection
    //      + spawn handler 协程"，这件事不是恢复某个已挂起的协程，所以用
    //      post_task 比较直接。
    // -------------------------------------------------------------------------
    void post_task(std::function<void()> f);

    // -------------------------------------------------------------------------
    // queue_boot_task —— 在 run() 启动 *之前* 预排一个任务
    // -------------------------------------------------------------------------
    // 用途：主线程要在 worker 启动后立刻执行某段代码（如"开始 listen + accept"），
    //      但主线程没有 ring 无法用 MSG_RING 通知。
    //      run() 启动后第一件事是 drain_cross_queue，从而拿到这些预排任务。
    //
    // 必须在 run() 启动前调用；启动后请用 post_task。
    // -------------------------------------------------------------------------
    void queue_boot_task(std::function<void()> f);

    // 给 awaiter 用：往 ready 队列添加 handle（仅限本线程）
    void push_ready(std::coroutine_handle<> h) {
        ready_.push_back(h);
    }

    IoUring& ring() noexcept { return *ring_; }
    TimerQueue& timer_queue() noexcept { return *timer_queue_; }

    // 固定注册缓冲池（可能为 nullptr：未启用或注册失败回退）。
    RegisteredBufferPool* buffer_pool() noexcept { return buf_pool_.get(); }

    // Timer 便捷接口（详见 timer/timer_queue.hpp）；同线程返回有效 TimerId，
    // 跨线程会 post_task bounce 但返回空 TimerId。
    TimerId run_after(std::chrono::nanoseconds delay, std::function<void()> fn);
    TimerId run_every(std::chrono::nanoseconds interval, std::function<void()> fn);
    void cancel(TimerId id);

    // thread_local current 指针：让 awaiter 不需要显式拿到 Scheduler 也能找到自己
    static Scheduler* current() noexcept { return tls_current_; }

private:
    void drain_cross_queue();
    void wake_remote();
    void rearm_eventfd_watch();

    static thread_local Scheduler* tls_current_;

    std::unique_ptr<IoUring> ring_;
    std::unique_ptr<TimerQueue> timer_queue_;
    std::unique_ptr<RegisteredBufferPool> buf_pool_;
    std::deque<std::coroutine_handle<>> ready_;
    std::atomic_bool stopping_{false};

    // 跨线程消息队列：handles 待恢复协程；tasks 待执行任务
    struct CrossQueue {
        std::mutex mu;
        std::vector<std::coroutine_handle<>> handles;
        std::vector<std::function<void()>> tasks;
    } cross_;

    // eventfd 用于无 ring 的外部线程唤醒（main 线程 / CoroThreadPool worker）
    int wake_fd_ = -1;
    // 永久监听 wake_fd 的 IoOperation；由 Scheduler 自己持有，run() 启动时挂、
    // 每次 CQE 后自动 rearm
    class EventfdWatcher;
    std::unique_ptr<EventfdWatcher> wake_watcher_;
};

}  // namespace coro_net

// SchedulerPool 已拆分至 coro_net/scheduler_pool.hpp。
