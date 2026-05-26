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
// 【为什么不用 SQPOLL】
//   IORING_SETUP_SQPOLL 让内核额外起一个 kthread 轮询 SQ tail，
//   省掉 io_uring_enter 系统调用，延迟最低。但代价：
//     - 占一个 CPU 核
//     - kernel < 5.13 需要 CAP_SYS_NICE 权限
//     - 调试时 ring 状态可能瞬时不一致，加大排错难度
//   教学项目优先清晰，不开 SQPOLL。
// =============================================================================

#pragma once

#include <liburing.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>

namespace coro_net {

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
    // entries：SQ/CQ 的容量。会被内核向上对齐到 2 的幂；建议 256~4096。
    //         队列满时 get_sqe() 返回 nullptr，调用方应先 submit() 再重试。
    explicit IoUring(unsigned entries) {
        io_uring_params params{};
        // 启用：
        //   IORING_SETUP_COOP_TASKRUN —— 完成事件不主动中断用户态，
        //                              省 IRQ 开销，几乎免费。
        //
        // 不启用 SINGLE_ISSUER：该 flag 要求"提交 SQE 的线程必须是创建 ring
        // 的线程"，否则 io_uring_enter 返回 -EEXIST。我们的 Scheduler 由
        // 主线程构造（构造时创建 ring）、由 worker 线程跑 run()（即提交
        // SQE），违反该约束。性能损失忽略不计（教学项目）。
        // 真正想用 SINGLE_ISSUER，得用 IORING_SETUP_R_DISABLED + 在 worker
        // 线程上调用 io_uring_enable_rings，本库后续优化阶段再考虑。
        params.flags = IORING_SETUP_COOP_TASKRUN;

        int ret = io_uring_queue_init_params(entries, &ring_, &params);
        if (ret < 0) {
            // ret 是负 errno
            throw std::system_error(-ret, std::system_category(),
                                    "io_uring_queue_init_params failed");
        }
        // 这里可以保存 params.features 字段，按需检测高阶特性是否可用
        features_ = params.features;
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

private:
    io_uring ring_{};
    uint32_t features_ = 0;
};

}  // namespace coro_net
