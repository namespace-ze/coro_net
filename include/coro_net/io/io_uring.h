// =============================================================================
// io/io_uring.h — io_uring 实例的 RAII 薄封装
// =============================================================================
//
// 【背景：io_uring 概念速查】
//
//   io_uring 是 Linux 5.1+ 引入的异步 IO 接口，作者 Jens Axboe。
//   核心思想：用户态和内核态共享两个 *无锁* 的环形队列：
//     - SQ (Submission Queue): 用户填提交项 SQE → 内核读
//     - CQ (Completion Queue): 内核填完成项 CQE → 用户读
//
//   "环形" 体现在：每个队列有 head/tail 两个下标，用户 / 内核分别推进它们，
//   下标对队列容量取模得到实际槽位。下标用 atomic_thread_fence 与对端同步。
//
//   一次完整的 IO 流程：
//     1) 用户态：从 SQ 取一个空闲 SQE 槽位 (io_uring_get_sqe)
//     2) 用户态：填好 opcode + fd + buf + len + user_data
//     3) 用户态：把 SQE 提交给内核 (io_uring_submit)
//     4) 内核：把 SQE 交给对应 IO 子系统（块/网络/timer）异步执行
//     5) 内核：IO 完成后把 CQE (res + flags + user_data 回传) 写入 CQ
//     6) 用户态：io_uring_wait_cqe 或 io_uring_peek_batch_cqe 拿到 CQE
//     7) 用户态：从 user_data 还原上下文，调用回调 / 恢复协程
//     8) 用户态：io_uring_cqe_seen 告知内核此 CQE 已处理（推进 head）
//
// 【本封装的职责】
//   - RAII 管理 io_uring 实例（构造时 queue_init，析构时 queue_exit）
//   - 暴露最常用 5 个操作的薄包装：get_sqe / submit / submit_and_wait /
//     peek_batch / cqe_seen
//   - 设置我们项目固定的 setup flags：COOP_TASKRUN + SINGLE_ISSUER
//     （Linux 5.18+，本环境内核 6.6 可用）
//
// 【setup flags 含义】
//   IORING_SETUP_COOP_TASKRUN：
//     内核完成 IO 后，*不* 主动中断用户态线程，而是等用户态线程下次进入内核
//     （或调用 io_uring_enter）时再批量处理。极大降低中断开销，特别适合
//     CPU 密集 + IO 密集混合负载。
//   IORING_SETUP_SINGLE_ISSUER：
//     声明这个 ring 只会被一个线程使用，内核可省去 ring 内部同步开销。
//     与我们的模型 A "每线程一个 ring" 完美契合。
//
// 【SQPOLL（优化阶段已启用，可配置）】
//   IORING_SETUP_SQPOLL 让内核额外起一个 kthread 轮询 SQ tail，
//   省掉 io_uring_enter 系统调用，延迟最低。代价是占一个 CPU 核。
//   为避免 N 个 worker = N 个轮询线程烧 N 核，我们用 IORING_SETUP_ATTACH_WQ
//   让多个 ring 共享同一个轮询线程（M 个轮询线程服务 N 个 ring，M<N）：
//   组首 ring 自带轮询线程，组内其余 ring 设 wq_fd = 组首 ring_fd 复用之。
//   sq_thread_idle 控制轮询线程空闲多久后休眠（休眠后下次 submit 由 liburing
//   通过 IORING_ENTER_SQ_WAKEUP 唤醒）。
//   kernel 6.6 下非特权即可用 SQPOLL；若环境拒绝（-EPERM）或拒绝某组合
//   （-EINVAL），构造函数会按候选 flag 列表依次回退，最差退回到普通 ring。
// =============================================================================

#pragma once

#include <liburing.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace coro_net {

// -----------------------------------------------------------------------------
// IoUringParams —— 构造 IoUring 的底层参数（与 SchedulerConfig 解耦）
// -----------------------------------------------------------------------------
struct IoUringParams {
    unsigned entries = 8192;          // SQ/CQ 容量（向上对齐到 2 的幂）
    bool     sqpoll = false;          // 是否启用 IORING_SETUP_SQPOLL
    unsigned sq_thread_idle_ms = 1000;// 轮询线程空闲多久（ms）后休眠
    int      wq_fd = -1;              // >=0：ATTACH_WQ 复用该 ring 的轮询线程
    int      sq_thread_cpu = -1;      // >=0：把本 ring 的 SQPOLL 线程钉到该核
                                      //      （IORING_SETUP_SQ_AFF；仅组首 leader 需设）
};

// -----------------------------------------------------------------------------
// IoUring —— 单 io_uring 实例
// -----------------------------------------------------------------------------
// 线程亲和：构造在哪个线程、就只在哪个线程使用（结合 SINGLE_ISSUER）。
// 例外：post() 跨线程恢复协程时，用 io_uring_prep_msg_ring 从其它线程的
//       ring 提交一个特殊 SQE 到本 ring，但这是"其它线程操作其它 ring，
//       目标只是把消息送到我"，不是别人直接操作我的 SQ/CQ。
// -----------------------------------------------------------------------------
class IoUring {
public:
    // 兼容旧调用：只给 entries（等价于无 SQPOLL 的普通 ring）。
    explicit IoUring(unsigned entries) : IoUring(IoUringParams{.entries = entries}) {}

    // 主构造：按 IoUringParams 组装 setup flags，失败时按候选列表优雅回退。
    //
    // 候选顺序（取第一个成功的）：
    //   1) 用户请求的完整 flag（COOP_TASKRUN [+ SQPOLL] [+ ATTACH_WQ]）
    //   2) 去掉 COOP_TASKRUN（个别内核拒绝 COOP_TASKRUN + SQPOLL 组合，-EINVAL）
    //   3) 去掉 SQPOLL/ATTACH_WQ，仅 COOP_TASKRUN（SQPOLL 被拒 -EPERM 时）
    //   4) 全部清零（最保守）
    //
    // 不启用 SINGLE_ISSUER：见文件头注释（ring 由主线程构造、worker 线程提交）。
    explicit IoUring(const IoUringParams& p) {
        unsigned full = IORING_SETUP_COOP_TASKRUN;
        if (p.sqpoll)                            full |= IORING_SETUP_SQPOLL;
        if (p.wq_fd >= 0)                        full |= IORING_SETUP_ATTACH_WQ;
        if (p.sqpoll && p.sq_thread_cpu >= 0)    full |= IORING_SETUP_SQ_AFF;

        // 候选按"保留最多有用特性"递减；关键是 SQ_AFF（仅钉核优化）要在 SQPOLL
        // 之前被剥离——否则内核拒绝 SQ_AFF 会把 SQPOLL 一起丢掉。
        std::vector<unsigned> candidates;
        candidates.push_back(full);                                   // 全开
        if (full & IORING_SETUP_SQ_AFF)
            candidates.push_back(full & ~IORING_SETUP_SQ_AFF);        // 丢钉核，保 SQPOLL
        if (full & IORING_SETUP_COOP_TASKRUN)
            candidates.push_back(full & ~IORING_SETUP_SQ_AFF
                                      & ~IORING_SETUP_COOP_TASKRUN);  // 再丢 COOP_TASKRUN
        // 最后退掉 SQPOLL（连带 ATTACH_WQ / SQ_AFF，它们都依赖 SQPOLL）
        candidates.push_back(IORING_SETUP_COOP_TASKRUN);
        candidates.push_back(0u);

        int ret = -EINVAL;
        for (unsigned flags : candidates) {
            io_uring_params params{};
            params.flags = flags;
            if (flags & IORING_SETUP_SQPOLL)
                params.sq_thread_idle = p.sq_thread_idle_ms;
            if (flags & IORING_SETUP_ATTACH_WQ)
                params.wq_fd = static_cast<unsigned>(p.wq_fd);
            if (flags & IORING_SETUP_SQ_AFF)
                params.sq_thread_cpu = static_cast<__u32>(p.sq_thread_cpu);

            ret = io_uring_queue_init_params(p.entries, &ring_, &params);
            if (ret == 0) {
                features_ = params.features;
                sqpoll_active_ = (flags & IORING_SETUP_SQPOLL) != 0;
                return;
            }
        }
        throw std::system_error(-ret, std::system_category(),
                                "io_uring_queue_init_params failed");
    }

    ~IoUring() {
        io_uring_queue_exit(&ring_);
    }

    IoUring(const IoUring&) = delete;
    IoUring& operator=(const IoUring&) = delete;

    // -------------------------------------------------------------------------
    // get_sqe —— 从 SQ 取一个空闲 SQE 槽位
    // -------------------------------------------------------------------------
    // 返回：可写的 SQE 指针；若 SQ 已满返回 nullptr。
    // 调用方应处理 nullptr：先 submit() 推进内核消费 SQ，然后重试。
    // 在 Scheduler::run() 的事件循环里我们通常会先批处理一次 submit，
    // 所以 nullptr 的情况很少出现。
    //
    // 注意：填完 SQE 后不会自动提交给内核，必须主动调用 submit()，
    // 直到那时 SQE 才被内核看到。
    // -------------------------------------------------------------------------
    io_uring_sqe* get_sqe() noexcept {
        return io_uring_get_sqe(&ring_);
    }

    // -------------------------------------------------------------------------
    // submit —— 把已填好的 SQE 一次性提交给内核
    // -------------------------------------------------------------------------
    // 这是一个 syscall（除非开了 SQPOLL）。内部会推进 SQ tail，
    // 并调用 io_uring_enter 让内核开始消费。
    // 返回值：成功提交的 SQE 数。
    // -------------------------------------------------------------------------
    int submit() noexcept {
        return io_uring_submit(&ring_);
    }

    // -------------------------------------------------------------------------
    // submit_and_wait —— 提交并等待至少 wait_nr 个完成
    // -------------------------------------------------------------------------
    // 用于事件循环的核心阻塞点：先把本轮积累的 SQE 推送给内核，
    // 然后阻塞直到至少有 wait_nr 个 CQE 可读（典型 wait_nr=1）。
    // 内部只做一次 syscall。
    // -------------------------------------------------------------------------
    int submit_and_wait(unsigned wait_nr) noexcept {
        return io_uring_submit_and_wait(&ring_, wait_nr);
    }

    // -------------------------------------------------------------------------
    // peek_batch_cqe —— 一次性取出 CQ 中所有就绪的 CQE
    // -------------------------------------------------------------------------
    // 参数 cqes: 输出数组指针；count: 数组容量。
    // 返回：实际填充的 CQE 数（≤ count）。
    // 不阻塞：CQ 空时返回 0。
    //
    // 用法：先 submit_and_wait(1) 保证至少 1 个 CQE 可读；
    //      然后 peek_batch_cqe 把这一批都取出来。
    //
    // 取完每个 CQE 必须调用 cqe_seen 推进 CQ head，否则后续 peek 会重复看到。
    // -------------------------------------------------------------------------
    unsigned peek_batch_cqe(io_uring_cqe** cqes, unsigned count) noexcept {
        return io_uring_peek_batch_cqe(&ring_, cqes, count);
    }

    // 推进 CQ head，告知内核这个 CQE 我已处理。
    void cqe_seen(io_uring_cqe* cqe) noexcept {
        io_uring_cqe_seen(&ring_, cqe);
    }

    // 一次性推进多个（peek_batch_cqe 返回数 N 后调用此）
    void cq_advance(unsigned n) noexcept {
        io_uring_cq_advance(&ring_, n);
    }

    // 给底层 io_uring* 指针；RegisteredFiles 等高级注册时要用
    io_uring* raw() noexcept { return &ring_; }

    // ring_fd（保留访问器，供后续诊断 / 监控用）
    int ring_fd() const noexcept { return ring_.ring_fd; }

    uint32_t features() const noexcept { return features_; }

    // SQPOLL 是否最终生效（可能因 -EPERM 回退而为 false）
    bool sqpoll_active() const noexcept { return sqpoll_active_; }

private:
    io_uring ring_{};
    uint32_t features_ = 0;
    bool     sqpoll_active_ = false;
};

}  // namespace coro_net
