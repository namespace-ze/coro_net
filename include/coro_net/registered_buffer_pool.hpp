// =============================================================================
// coro_net/registered_buffer_pool.hpp —— 每 worker 的固定注册缓冲池
// =============================================================================
//
// 【目的】
//   用 io_uring_register_buffers 把一块连续 arena 切成 capacity 个固定大小 slot
//   一次性注册（pin）进内核。之后用 io_uring_prep_read_fixed / write_fixed +
//   buf_index 收发，内核免去每次 IO 的 get_user_pages，达到零拷贝。
//
// 【模型】
//   每连接生命周期借一个 slot（acquire），关闭时归还（release）。
//   slot 既是 recv 落点，也可直接作为 send 源（echo 零拷贝回显）。
//   池容量需 ≥ 单 worker 最大并发连接数；池满时 acquire 返回 -1，调用方回退
//   普通 recv/send（堆 Buffer）。
//
// 【限制】
//   - 注册内存被 pin，计入 RLIMIT_MEMLOCK：capacity*slot_size 必须 ≤ memlock。
//     memlock 不足时 register_buffers 返回 -ENOMEM/-EPERM → enabled()=false，回退。
//   - 单 ring 注册上限 IORING_MAX_REG_BUFFERS = 16384，故 capacity 应 ≤ 该值。
//
// 【线程亲和】
//   注册是一次性 setup syscall，可在 ring 所属 Scheduler 构造时（主线程）完成；
//   acquire/release/slot_ptr 只在该 worker 线程内调用（无锁 free-list）。
//
// 本头文件含 <liburing.h>，仅供 .cc 引入（不进对外轻量头链）。
// =============================================================================

#pragma once

#include <liburing.h>
#include <cstddef>
#include <cstdlib>
#include <vector>

namespace coro_net {

// 单 ring 注册 buffer 上限（uapi: IORING_MAX_REG_BUFFERS = 1U<<14）。
inline constexpr unsigned kMaxRegBuffers = 1u << 14;  // 16384

class RegisteredBufferPool {
public:
    // 在 ring 上注册 capacity 个 slot_size 字节的固定缓冲。
    RegisteredBufferPool(io_uring* ring, unsigned capacity, unsigned slot_size)
        : ring_(ring), slot_size_(slot_size), capacity_(capacity) {
        if (capacity_ == 0 || slot_size_ == 0) return;
        if (capacity_ > kMaxRegBuffers) capacity_ = kMaxRegBuffers;

        const std::size_t total =
            static_cast<std::size_t>(capacity_) * slot_size_;
        // 页对齐分配，利于 pin。
        void* p = nullptr;
        if (::posix_memalign(&p, 4096, total) != 0 || p == nullptr) return;
        arena_ = static_cast<char*>(p);

        std::vector<iovec> iovs(capacity_);
        for (unsigned i = 0; i < capacity_; ++i) {
            iovs[i].iov_base = arena_ + static_cast<std::size_t>(i) * slot_size_;
            iovs[i].iov_len  = slot_size_;
        }

        int ret = io_uring_register_buffers(ring_, iovs.data(), capacity_);
        if (ret < 0) {
            // 常见：-ENOMEM/-EPERM（memlock 不足）。释放 arena，置不可用。
            std::free(arena_);
            arena_ = nullptr;
            reg_errno_ = -ret;
            return;
        }

        free_list_.reserve(capacity_);
        for (int i = static_cast<int>(capacity_) - 1; i >= 0; --i)
            free_list_.push_back(i);
        enabled_ = true;
    }

    ~RegisteredBufferPool() {
        if (enabled_) io_uring_unregister_buffers(ring_);
        if (arena_) std::free(arena_);
    }

    RegisteredBufferPool(const RegisteredBufferPool&) = delete;
    RegisteredBufferPool& operator=(const RegisteredBufferPool&) = delete;

    bool     enabled()   const noexcept { return enabled_; }
    unsigned slot_size() const noexcept { return slot_size_; }
    unsigned capacity()  const noexcept { return capacity_; }
    // 注册失败时的 errno（>0），用于日志诊断；成功为 0。
    int      reg_errno() const noexcept { return reg_errno_; }

    // 借一个 slot，返回下标；池空/未启用返回 -1。
    int acquire() noexcept {
        if (!enabled_ || free_list_.empty()) return -1;
        int idx = free_list_.back();
        free_list_.pop_back();
        return idx;
    }

    // 归还（idx<0 视为未借，忽略）。
    void release(int idx) noexcept {
        if (idx < 0) return;
        free_list_.push_back(idx);
    }

    char* slot_ptr(int idx) noexcept {
        return arena_ + static_cast<std::size_t>(idx) * slot_size_;
    }

private:
    io_uring*        ring_;
    char*            arena_ = nullptr;
    unsigned         slot_size_ = 0;
    unsigned         capacity_ = 0;
    std::vector<int> free_list_;   // 空闲下标栈
    bool             enabled_ = false;
    int              reg_errno_ = 0;
};

}  // namespace coro_net
