// =============================================================================
// coro_net/scheduler.hpp — 单 IO worker 的调度器
// =============================================================================
//
// 【角色定位】
//
//   每个 IO worker 线程持有一个 Scheduler 实例。Scheduler 是这个线程的"大脑"：
//     - 拥有一个 io_uring 实例（SQ/CQ 不与其他线程共享，完全无锁）
//     - 拥有一个 BufferRing 池（provide-buffers 入口）
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
#include <coroutine>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>
#include <vector>
#include <thread>

namespace coro_net {

class IoUring;
class BufferRing;

// -----------------------------------------------------------------------------
// SchedulerConfig：构造 Scheduler 时的参数
// -----------------------------------------------------------------------------
struct SchedulerConfig {
    unsigned ring_entries = 1024;       // SQ/CQ 容量
    uint16_t buf_ring_bgid = 1;          // 默认 buffer group id
    uint16_t buf_ring_entries = 1024;    // buffer 数量
    uint32_t buf_ring_buf_size = 4096;   // 每个 buffer 字节数
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
    BufferRing& buffer_ring() noexcept { return *brg_; }

    // thread_local current 指针：让 awaiter 不需要显式拿到 Scheduler 也能找到自己
    static Scheduler* current() noexcept { return tls_current_; }

private:
    void drain_cross_queue();
    void wake_remote();
    void rearm_eventfd_watch();

    static thread_local Scheduler* tls_current_;

    std::unique_ptr<IoUring> ring_;
    std::unique_ptr<BufferRing> brg_;
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

// =============================================================================
// SchedulerPool —— 管理 N 个 Scheduler（每个独占一个线程）
// =============================================================================
//
// 【用途】TcpServer 启动时构造一个 SchedulerPool，每个 worker 线程跑一个
//        Scheduler::run()。新连接到来时 round-robin 选下一个 worker。
//
// 【接口】
//   - start()：拉起 N 个线程，每个跑一个 Scheduler::run()
//   - stop_all()：让所有 Scheduler 退出循环
//   - wait()：join 所有线程
//   - next()：round-robin 选下一个 Scheduler 引用
//   - at(i)：拿到第 i 个 Scheduler 引用
//
// 【生命周期】
//   构造时 *仅* 创建 Scheduler 对象（在主线程的栈上构造，未运行）；
//   start() 拉起线程；析构前必须 stop_all + wait。
// =============================================================================
class SchedulerPool {
public:
    explicit SchedulerPool(size_t n);
    ~SchedulerPool();

    SchedulerPool(const SchedulerPool&) = delete;
    SchedulerPool& operator=(const SchedulerPool&) = delete;

    void start();
    void stop_all();
    void wait();

    size_t size() const noexcept { return schedulers_.size(); }
    Scheduler& at(size_t i) noexcept { return *schedulers_[i]; }
    Scheduler& next() noexcept;

private:
    std::vector<std::unique_ptr<Scheduler>> schedulers_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_idx_{0};
    bool started_ = false;
};

}  // namespace coro_net
