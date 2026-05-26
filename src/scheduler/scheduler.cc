// =============================================================================
// scheduler.cc — Scheduler 事件循环实现
// =============================================================================
// SchedulerPool 拆出到 scheduler_pool.cc。
// EventfdWatcher 是 Scheduler 的私有嵌套类，留在本文件。
// =============================================================================
#include "coro_net/scheduler.hpp"
#include "coro_net/io_operation.hpp"
#include "coro_net/io/io_uring.h"
#include "coro_net/log.hpp"
#include "coro_net/timer/timer_id.hpp"
#include "coro_net/timer/timer_queue.hpp"

// EventfdWatcher 定义放到 .cc 里（私有实现）

#include <liburing.h>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <cstring>
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
// 构造：建 ring + eventfd watcher + TimerQueue
// -----------------------------------------------------------------------------
Scheduler::Scheduler(SchedulerConfig cfg) {
    ring_ = std::make_unique<IoUring>(cfg.ring_entries);
    // eventfd：用于跨线程唤醒。EFD_NONBLOCK 让 read 不阻塞；
    // 读到 0 也无所谓——我们用 io_uring 异步读，syscall 行为在这里不重要。
    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    wake_watcher_ = std::make_unique<EventfdWatcher>(*this, wake_fd_);
    timer_queue_ = std::make_unique<TimerQueue>(*this);
}

Scheduler::~Scheduler() {
    timer_queue_.reset();   // 先关 timerfd（释放它在 ring 上挂的 read）
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
// wake_remote —— 用 eventfd 唤醒目标 Scheduler（统一路径）
// -----------------------------------------------------------------------------
// 历史上分两条：本线程是另一个 Scheduler 则走 MSG_RING；无 ring 则走 eventfd。
// 现统一为 eventfd：路径单一、CQE 处理少一种特殊分支，与 shared-nothing
// per-worker 模型更一致。代价：每次唤醒多一次 write(eventfd, 8B) syscall。
// -----------------------------------------------------------------------------
void Scheduler::wake_remote() {
    if (tls_current_ == this) return;
    uint64_t v = 1;
    ssize_t r = ::write(wake_fd_, &v, sizeof(v));
    (void)r;  // eventfd 不会丢消息（counter 累加语义），EAGAIN 也无所谓
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
    LOG_INFO << "Scheduler@" << static_cast<const void*>(this) << " entering run()";

    // 挂一个永久 read 在 wake_fd 上，作为唤醒入口
    rearm_eventfd_watch();
    // TimerQueue 在本线程挂上自己的 timerfd 读
    timer_queue_->start();

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
            LOG_ERROR << "Scheduler@" << static_cast<const void*>(this)
                      << " submit_and_wait returned " << r
                      << " (errno=" << -r << ": " << std::strerror(-r) << ")";
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

            // user_data 为 nullptr：stop() 发的 NOP 唤醒 → 跳过
            if (!data) continue;

            auto* op = static_cast<IoOperationBase*>(data);
            op->on_complete(cqe->res, cqe->flags);
        }
        ring_->cq_advance(n);
    }

    LOG_INFO << "Scheduler@" << static_cast<const void*>(this) << " exiting run()";
    tls_current_ = nullptr;
}

// -----------------------------------------------------------------------------
// Timer 便捷接口 —— 详见 timer_queue.hpp
// -----------------------------------------------------------------------------
TimerId Scheduler::run_after(std::chrono::nanoseconds delay, std::function<void()> fn) {
    if (tls_current_ == this) {
        return timer_queue_->add(std::move(fn), delay);
    }
    // 跨线程：bounce 到本 worker 添加；调用方拿不到 id
    post_task([this, fn = std::move(fn), delay]() mutable {
        timer_queue_->add(std::move(fn), delay);
    });
    return TimerId{};
}

TimerId Scheduler::run_every(std::chrono::nanoseconds interval, std::function<void()> fn) {
    if (tls_current_ == this) {
        return timer_queue_->add_periodic(std::move(fn), interval);
    }
    post_task([this, fn = std::move(fn), interval]() mutable {
        timer_queue_->add_periodic(std::move(fn), interval);
    });
    return TimerId{};
}

void Scheduler::cancel(TimerId id) {
    if (tls_current_ == this) {
        timer_queue_->cancel(id);
        return;
    }
    post_task([this, id]() { timer_queue_->cancel(id); });
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
        // 跨线程：统一走 eventfd 唤醒
        wake_remote();
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

}  // namespace coro_net
