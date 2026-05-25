// =============================================================================
// scheduler.cc — Scheduler 事件循环 + SchedulerPool 实现
// =============================================================================
#include "coro_net/scheduler.hpp"
#include "coro_net/io_operation.hpp"
#include "coro_net/io/io_uring.h"
#include "coro_net/io/buffer_ring.h"

// EventfdWatcher 定义放到 .cc 里（私有实现）

#include <liburing.h>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <cstring>
#include <cstdio>
#include <sys/eventfd.h>
#include <unistd.h>

namespace coro_net {

thread_local Scheduler* Scheduler::tls_current_ = nullptr;

// -----------------------------------------------------------------------------
// EventfdWatcher —— 永久监听 wake_fd 的 IoOperationBase 子类
// -----------------------------------------------------------------------------
//
// 【用途】允许"没有 ring 的线程"（main 线程、CoroThreadPool worker 等）通过
//        eventfd_write(wake_fd, 1) 唤醒目标 Scheduler。
//
// 【实现要点】
//   1. 不是协程的 awaiter（没有协程在等它），所以 handle_ 永远是空。
//   2. on_complete 中收到一次"被读到的 8 字节"后，立刻 re-submit 自己，
//      继续等待下次唤醒。
//   3. 数据本身（8 字节）不关心，读取就只是为了清掉 eventfd 计数。
// -----------------------------------------------------------------------------
class Scheduler::EventfdWatcher : public IoOperationBase {
public:
    EventfdWatcher(Scheduler& s, int fd) : IoOperationBase(s), fd_(fd) {}

    void rearm(io_uring_sqe* sqe) {
        // 重新提交 read(wake_fd, &dummy_, 8)
        io_uring_prep_read(sqe, fd_, &dummy_, sizeof(dummy_), 0);
        io_uring_sqe_set_data(sqe, this);
    }

    // 每次 CQE 到来：re-arm 自己（继续监听下一次唤醒）
    void on_complete(int32_t /*res*/, uint32_t /*flags*/) noexcept override {
        // 用本线程 ring 重新提交（注意：本函数在 Scheduler 线程的 CQE 处理路径上）
        io_uring_sqe* sqe = sched_->ring().get_sqe();
        if (sqe) rearm(sqe);
        // 不 push 任何协程 handle；唤醒效果由 wait_cqe 返回自然实现
    }

private:
    int fd_;
    uint64_t dummy_ = 0;  // 8 字节缓冲，给 eventfd read 用
};

// -----------------------------------------------------------------------------
// 构造：建 ring + buffer ring
// -----------------------------------------------------------------------------
Scheduler::Scheduler(SchedulerConfig cfg) {
    ring_ = std::make_unique<IoUring>(cfg.ring_entries);
    brg_ = std::make_unique<BufferRing>(ring_->raw(),
                                        cfg.buf_ring_bgid,
                                        cfg.buf_ring_entries,
                                        cfg.buf_ring_buf_size);
    // eventfd：用于无 ring 线程唤醒。EFD_NONBLOCK 让 read 不阻塞；
    // 读到 0 也无所谓——我们用 io_uring 异步读，syscall 行为在这里不重要。
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    wake_watcher_ = std::make_unique<EventfdWatcher>(*this, wake_fd_);
}

Scheduler::~Scheduler() {
    if (wake_fd_ >= 0) ::close(wake_fd_);
}

// 在 run() 启动时调用：第一次把 watcher 挂上 ring，CQE 回来后它会自己 rearm
void Scheduler::rearm_eventfd_watch() {
    io_uring_sqe* sqe = ring_->get_sqe();
    if (sqe) wake_watcher_->rearm(sqe);
}

// -----------------------------------------------------------------------------
// drain_cross_queue —— 处理跨线程投来的恢复请求与任务
// -----------------------------------------------------------------------------
//
// 【调用上下文】只在 Scheduler 自己的线程内调用。
//
// 【做法】
//   1. 加锁、把两个 vector swap 出来（最小化临界区），解锁
//   2. handles 直接 push 到本地 ready_，统一调度
//   3. tasks 直接执行（这些任务通常是"在我这边 spawn 一个协程"，
//      执行时它们再 push 协程 handle 到 ready_）
// -----------------------------------------------------------------------------
void Scheduler::drain_cross_queue() {
    std::vector<std::coroutine_handle<>> h_tmp;
    std::vector<std::function<void()>> t_tmp;
    {
        std::lock_guard<std::mutex> lk(cross_.mu);
        h_tmp.swap(cross_.handles);
        t_tmp.swap(cross_.tasks);
    }
    for (auto h : h_tmp) {
        if (h && !h.done()) ready_.push_back(h);
    }
    for (auto& f : t_tmp) {
        f();
    }
}

// -----------------------------------------------------------------------------
// wake_remote —— 从当前线程的 ring 发一个 NOP MSG_RING 到目标 ring 上
// -----------------------------------------------------------------------------
//
// 【为什么需要这一步】
//   假设目标 Scheduler 正阻塞在 submit_and_wait(1) 里，cross_.queue 多了一项
//   它也感知不到。所以我们需要在目标 ring 上"种"一个 CQE 让 wait 返回。
//
// 【MSG_RING 工作模型】
//
//   thread A (src)                                     thread B (target)
//   --------------                                     -----------------
//   io_uring_prep_msg_ring(sqe, B_ring_fd, 0, 0, 0)
//   submit                                             正阻塞在 submit_and_wait
//        |                                                      |
//        +--→ 内核：把一个 CQE 推到 B 的 CQ                       |
//        |    (res=0, user_data=0)                              |
//        |                                              ←--- 醒来
//        |                                              处理 CQE：user_data=0 跳过
//        +--→ 给 A 的 CQ 推一个 CQE 表示 MSG_RING 提交完成        |
//                                                       drain_cross_queue 取消息
//
//   data 字段我们填 0（不真正传递任何数据），跨线程"内容"通过 cross_.handles /
//   cross_.tasks 队列传递。MSG_RING 只是"敲门"。
//
// 【调用者必须满足】
//   - tls_current_ != this（不是本线程自己 post 自己）
//   - tls_current_ != nullptr（调用线程有自己的 ring；若没有 ring，
//     这是 S6 阶段 CoroThreadPool 才会遇到的场景，本函数当前不处理）
// -----------------------------------------------------------------------------
void Scheduler::wake_remote() {
    Scheduler* src = tls_current_;
    if (src == this) return;

    if (src) {
        // 调用线程是另一个 Scheduler → 用 MSG_RING（无锁、零内存拷贝）
        io_uring_sqe* sqe = src->ring().get_sqe();
        if (!sqe) {
            src->ring().submit();
            sqe = src->ring().get_sqe();
            if (!sqe) return;
        }
        io_uring_prep_msg_ring(sqe, ring_->ring_fd(), 0, 0, 0);
        io_uring_sqe_set_data(sqe, nullptr);  // 源端 CQE 忽略
        src->ring().submit();
    } else {
        // 调用线程没有 ring（main 线程 / 业务线程）→ 走 eventfd 兜底
        uint64_t v = 1;
        ssize_t r = ::write(wake_fd_, &v, sizeof(v));
        (void)r;  // EAGAIN 也无所谓：之前的写还没被消费就足以唤醒
    }
}

// -----------------------------------------------------------------------------
// post / post_task：本线程 vs 跨线程
// -----------------------------------------------------------------------------
void Scheduler::post(std::coroutine_handle<> h) {
    if (tls_current_ == this) {
        push_ready(h);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(cross_.mu);
        cross_.handles.push_back(h);
    }
    wake_remote();
}

void Scheduler::post_task(std::function<void()> f) {
    if (tls_current_ == this) {
        // 本线程直接执行（保持顺序简单）
        f();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(cross_.mu);
        cross_.tasks.push_back(std::move(f));
    }
    wake_remote();
}

void Scheduler::queue_boot_task(std::function<void()> f) {
    // 仅在 run() 启动前调用（无锁竞争）；如果误在启动后调用，仍是线程安全的，
    // 但调用方需自己保证最终有人 wake_remote（或本任务自己包含 await）。
    std::lock_guard<std::mutex> lk(cross_.mu);
    cross_.tasks.push_back(std::move(f));
}

// -----------------------------------------------------------------------------
// run —— 事件循环
// -----------------------------------------------------------------------------
void Scheduler::run() {
    tls_current_ = this;
    stopping_.store(false, std::memory_order_relaxed);

    // 挂一个永久 read 在 wake_fd 上，作为唤醒入口
    rearm_eventfd_watch();

    while (!stopping_.load(std::memory_order_relaxed)) {
        // 1) 先吃掉跨线程投来的消息（handles 进 ready_、tasks 立刻跑）
        drain_cross_queue();

        // 2) 处理 ready 队列
        while (!ready_.empty()) {
            auto h = ready_.front();
            ready_.pop_front();
            if (h && !h.done()) h.resume();
        }

        if (stopping_.load(std::memory_order_relaxed)) break;

        // 3) 提交 + 阻塞等至少 1 个 CQE
        int r = ring_->submit_and_wait(1);
        if (r < 0 && r != -EINTR) {
            std::fprintf(stderr,
                "[Scheduler %p] submit_and_wait returned %d (errno-meaning: %s)\n",
                (void*)this, r, std::strerror(-r));
            throw std::system_error(-r, std::system_category(),
                                    "io_uring submit_and_wait failed");
        }

        // 4) 批量处理 CQE
        constexpr unsigned kBatch = 64;
        io_uring_cqe* cqes[kBatch];
        unsigned n = ring_->peek_batch_cqe(cqes, kBatch);

        for (unsigned i = 0; i < n; ++i) {
            io_uring_cqe* cqe = cqes[i];
            void* data = io_uring_cqe_get_data(cqe);

            // user_data 为 nullptr：典型来源
            //   (a) stop() 发的 NOP 唤醒
            //   (b) wake_remote 提交 MSG_RING 后 src ring 的 CQE
            //   (c) wake_remote 落到 target ring 的 CQE（data 我们填的 0）
            // 一律跳过。
            if (!data) continue;

            auto* op = static_cast<IoOperationBase*>(data);
            op->on_complete(cqe->res, cqe->flags);
        }
        ring_->cq_advance(n);
    }

    tls_current_ = nullptr;
}

// -----------------------------------------------------------------------------
// stop —— 通知 run 退出
// -----------------------------------------------------------------------------
void Scheduler::stop() {
    bool was_running = !stopping_.exchange(true, std::memory_order_relaxed);
    if (!was_running) return;

    if (tls_current_ == this) {
        // 本线程：放一个 NOP SQE 让循环立刻醒来，下一轮检查 stopping_ 退出
        io_uring_sqe* sqe = ring_->get_sqe();
        if (sqe) {
            io_uring_prep_nop(sqe);
            io_uring_sqe_set_data(sqe, nullptr);
            ring_->submit();
        }
    } else {
        // 跨线程：用 MSG_RING 唤醒；wake_remote 期望 src 有 ring
        if (tls_current_) {
            wake_remote();
        }
        // 如果调用方完全没有 ring（如 main thread），目标 worker 可能仍卡在 wait。
        // S6 阶段补 eventfd 兜底；S4 阶段 TcpServer 内部会在合适的"有 ring"
        // 上下文调用 stop。
    }
}

// -----------------------------------------------------------------------------
// spawn —— 启动 Task<void> 协程
// -----------------------------------------------------------------------------
namespace {
FireAndForget spawn_shim(Task<void> t) {
    co_await std::move(t);
    co_return;
}
}

void Scheduler::spawn(Task<void> task) {
    spawn_shim(std::move(task));
}

// =============================================================================
// SchedulerPool 实现
// =============================================================================

SchedulerPool::SchedulerPool(size_t n) {
    schedulers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        schedulers_.emplace_back(std::make_unique<Scheduler>());
    }
}

SchedulerPool::~SchedulerPool() {
    if (started_) {
        stop_all();
        wait();
    }
}

void SchedulerPool::start() {
    started_ = true;
    threads_.reserve(schedulers_.size());
    for (auto& sch : schedulers_) {
        Scheduler* p = sch.get();
        threads_.emplace_back([p] { p->run(); });
    }
}

void SchedulerPool::stop_all() {
    // 注意：本函数通常在主线程（没有 ring 的线程）调用。
    // Scheduler::stop 在跨线程 + 无 ring 情况下不能唤醒目标。
    // 解决：我们让每个 worker 自己定期检查 stopping_ —— 不行，那需要 timeout。
    // 简化做法：要求 stop_all 在某个 scheduler 线程上调用（如把它包成
    // 一个发给 worker[0] 的 task）。
    //
    // 但更常见的用法是主线程 join 等结束，我们这里做一个权宜：
    // 在每个 scheduler 自己 ring 上 *用一个 MSG_RING SQE 从我们这条线程* —
    // 但我们没 ring 啊...
    //
    // 解决方案：每个 worker 在自己线程内监听一个"stop 信号"。最简单的实现：
    // 在每个 scheduler 上 spawn 一个监听 stop 的协程。但这又要协程。
    //
    // 为 S4 简化：要求调用方先把所有 worker 都引导到一个 "drain → stop" 协程
    // （由用户代码控制何时停）。stop_all() 直接设标志，依赖
    // worker 自己 spawn 的协程会在适当时机调用本 scheduler 的 stop()。
    //
    // 这里只设置每个 scheduler 的 stopping_ 标志；MSG_RING 唤醒在 S6/兜底里做。
    for (auto& sch : schedulers_) {
        sch->stop();   // 同进程内 stop()，跨线程时只设标志，依赖现有 CQE 唤醒
    }
}

void SchedulerPool::wait() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    started_ = false;
}

Scheduler& SchedulerPool::next() noexcept {
    size_t idx = next_idx_.fetch_add(1, std::memory_order_relaxed) %
                 schedulers_.size();
    return *schedulers_[idx];
}

}  // namespace coro_net
