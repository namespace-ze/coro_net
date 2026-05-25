// =============================================================================
// io/buffer_ring.h — io_uring provide-buffers 池
// =============================================================================
//
// 【provide-buffers 解决的问题】
//
//   传统 recv 必须预先把 buffer 指针传给内核（read/recv syscall 或 SQE.addr）。
//   高并发 server 持有 N 个连接，必须 N 个 buffer 才能并发收包；
//   N=10万时 buffer 占用 = 10万 * 4KB = 400MB，浪费严重。
//
//   provide-buffers 把"准备 buffer"与"提交 recv"解耦：
//     1) 用户提前注册一个 buffer 组（M 个 buffer，M << N）给内核
//     2) 发起 recv 时设置 IOSQE_BUFFER_SELECT，*不指定* 具体 buffer
//     3) 内核在 IO 就绪时从组里挑一个空闲 buffer 写入
//     4) CQE 返回时通过 flags 字段告知用了哪个 buffer (bid)
//     5) 用户处理完后归还该 bid，buffer 重新进入空闲池
//
//   收益：N 个空闲连接共享 M 个 buffer，内存 O(M) 而非 O(N)。
//
// 【两种实现：PROVIDE_BUFFERS opcode vs REGISTER_PBUF_RING】
//
//   旧方案 (5.7+)：每次补充 buffer 都要提交一个 OP_PROVIDE_BUFFERS SQE，
//                  代码繁琐、性能一般。
//
//   新方案 (5.19+)：REGISTER_PBUF_RING 一次性把整个 buffer ring 注册给内核，
//                   内核直接从 ring 自己取下一个空闲；用户通过推进 tail 来
//                   "归还"。本库用新方案。
//
// 【数据结构】
//
//     io_uring_buf_ring (内核侧视图)
//     +--------+--------+--------+--------+
//     | buf[0] | buf[1] | buf[2] | buf[3] |     <-- mmap 出来的共享数组
//     +--------+--------+--------+--------+
//                                    ^
//                                    tail（用户更新，表示"我又放了一个 buffer 进来"）
//
//     每个 buf 项是 io_uring_buf:
//        addr  : 实际 buffer 的用户态地址
//        len   : buffer 大小
//        bid   : buffer id（用户分配，CQE 回来时通过 flags 中的 bid 告诉你用了哪个）
//        resv  : 保留
//
//     内核挑 buffer 的策略：从 head 取一个、head++。
//     用户归还 buffer：把那个 buffer 信息再次写到 tail 槽位、tail++。
//     初始化时所有 buffer 都"已归还"，所以 tail 直接推到 entries。
//
// 【bid 的语义】
//
//   bid 是 16 位整数，作为 buffer 的逻辑 ID。
//   - 我们让 bid = buffer 在底层数组中的下标，简化归还逻辑。
//   - CQE 中通过 (cqe->flags >> IORING_CQE_BUFFER_SHIFT) 拿到 bid。
//   - 归还时把这个 bid 重新写到 ring 的 tail。
//
// 【与 per-conn Buffer 的协作】
//
//   本库选择"立即归还"策略（见 plan §Buffer 协作）：
//     auto bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
//     auto data = brg.view(bid).first(cqe->res);
//     conn->buf_.append(data);     // memcpy 到 per-conn 用户态 Buffer
//     brg.return_buffer(bid);      // 立刻归还，buffer ring 不长期占用
//   这样 BufferRing 只是 recv 路径上的"中转站"，
//   per-conn Buffer 仍保留 mymuduo 风格的 peek/retrieve 接口便于拆帧。
// =============================================================================

#pragma once

#include <liburing.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace coro_net {

class BufferRing {
public:
    // -------------------------------------------------------------------------
    // 构造：在指定 io_uring 上注册一个 buffer ring
    // -------------------------------------------------------------------------
    // 参数：
    //   ring     : 关联的 IoUring（其 raw() 是 io_uring*）
    //   bgid     : buffer group ID，发起 recv 时 SQE.buf_group 要填这个，
    //              内核据此知道从哪个组挑 buffer。
    //              同一个 ring 可以注册多个 bgid 的组。
    //   entries  : buffer 数量，必须是 2 的幂（向上取整）。
    //   buf_size : 每个 buffer 的字节数。
    //
    // 内存布局：内部 alloc 一片连续的 `entries * buf_size` 字节作 buffer pool。
    //          + 一个 io_uring_buf_ring 控制结构。
    // -------------------------------------------------------------------------
    BufferRing(io_uring* ring, uint16_t bgid, uint16_t entries, uint32_t buf_size)
        : ring_(ring), bgid_(bgid), entries_(entries), buf_size_(buf_size) {
        if ((entries & (entries - 1)) != 0) {
            throw std::invalid_argument("BufferRing entries must be power of two");
        }
        // 1) 分配 buffer 数据区
        bufs_.resize(static_cast<size_t>(entries) * buf_size);
        // 2) 调用 liburing 提供的便捷函数注册一个 buf_ring 给内核
        //    flags = 0；返回内核侧映射好的 io_uring_buf_ring*
        int err = 0;
        br_ = io_uring_setup_buf_ring(ring_, entries, bgid, 0, &err);
        if (!br_) {
            throw std::system_error(-err, std::system_category(),
                                    "io_uring_setup_buf_ring failed");
        }
        // 3) 把所有 buffer 推进 ring："tail 推到 entries"等价于全部已归还
        for (uint16_t i = 0; i < entries; ++i) {
            io_uring_buf_ring_add(br_,
                                  bufs_.data() + static_cast<size_t>(i) * buf_size,
                                  buf_size,
                                  i,                       // bid = i
                                  io_uring_buf_ring_mask(entries),
                                  i);                      // buf_offset = i
        }
        // 4) advance：让内核看到这些新归还的 buffer（mask 内的提交点）
        io_uring_buf_ring_advance(br_, entries);
    }

    ~BufferRing() {
        if (br_) {
            io_uring_free_buf_ring(ring_, br_, entries_, bgid_);
        }
    }

    BufferRing(const BufferRing&) = delete;
    BufferRing& operator=(const BufferRing&) = delete;

    // -------------------------------------------------------------------------
    // return_buffer —— 把指定 bid 的 buffer 归还给内核
    // -------------------------------------------------------------------------
    // 调用时机：CQE 处理完，用户已把数据 append 到 per-conn Buffer。
    // 操作：把这个 buffer 信息重新写到 ring 的下一个 tail 槽，然后 advance(1)
    //       唤起内核重新使用。
    // O(1) 摊销。
    // -------------------------------------------------------------------------
    void return_buffer(uint16_t bid) noexcept {
        char* addr = bufs_.data() + static_cast<size_t>(bid) * buf_size_;
        io_uring_buf_ring_add(br_, addr, buf_size_, bid,
                              io_uring_buf_ring_mask(entries_), 0);
        io_uring_buf_ring_advance(br_, 1);
    }

    // 查看 bid 对应的 buffer 字节区域；不持有所有权，只读 view。
    std::span<char> view(uint16_t bid) noexcept {
        return {bufs_.data() + static_cast<size_t>(bid) * buf_size_, buf_size_};
    }

    uint16_t group_id() const noexcept { return bgid_; }
    uint16_t entries() const noexcept { return entries_; }
    uint32_t buf_size() const noexcept { return buf_size_; }

private:
    io_uring* ring_;
    uint16_t bgid_;
    uint16_t entries_;
    uint32_t buf_size_;
    io_uring_buf_ring* br_ = nullptr;
    std::vector<char> bufs_;
};

}  // namespace coro_net
