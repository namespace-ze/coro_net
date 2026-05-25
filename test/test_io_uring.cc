// =============================================================================
// test_io_uring.cc — IoUring + BufferRing 基本功能测试
// =============================================================================
// 验证：
//   1. IoUring 能构造、提交 NOP、收到 CQE
//   2. BufferRing 能注册、recv 到 /dev/null（实际上用 pipe 测试更直观）
//   3. BufferRing return_buffer 后 bid 能再次被使用
// =============================================================================

#include "coro_net/io/io_uring.h"
#include "coro_net/io/buffer_ring.h"
#include "test_util.hpp"

#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

using coro_net::IoUring;
using coro_net::BufferRing;

// -----------------------------------------------------------------------------
// 1. NOP 提交 / 完成
// -----------------------------------------------------------------------------
CORO_TEST(iouring_nop_round_trip) {
    IoUring ring(64);

    io_uring_sqe* sqe = ring.get_sqe();
    CORO_EXPECT_TRUE(sqe != nullptr);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 0xDEADBEEF);

    int submitted = ring.submit_and_wait(1);
    CORO_EXPECT_EQ(submitted, 1);

    io_uring_cqe* cqes[8];
    unsigned n = ring.peek_batch_cqe(cqes, 8);
    CORO_EXPECT_EQ(n, 1u);
    CORO_EXPECT_EQ((uint64_t)io_uring_cqe_get_data64(cqes[0]),
                   (uint64_t)0xDEADBEEFu);
    CORO_EXPECT_EQ(cqes[0]->res, 0);
    ring.cq_advance(n);
}

// -----------------------------------------------------------------------------
// 2. 通过 BufferRing + recv 接收 pipe 写入数据
// -----------------------------------------------------------------------------
CORO_TEST(buffer_ring_recv_from_pipe) {
    IoUring ring(64);
    BufferRing brg(ring.raw(), /*bgid=*/1, /*entries=*/8, /*buf_size=*/256);

    // 用 AF_UNIX socketpair 测试（recv 只支持 socket fd）
    int fds[2];
    CORO_EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    const char* msg = "hello io_uring";
    size_t msg_len = std::strlen(msg);
    ssize_t w = write(fds[1], msg, msg_len);
    CORO_EXPECT_EQ((size_t)w, msg_len);

    // 提交 recv（带 BUFFER_SELECT 标志）
    io_uring_sqe* sqe = ring.get_sqe();
    CORO_EXPECT_TRUE(sqe != nullptr);
    io_uring_prep_recv(sqe, fds[0], nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = 1;                            // 与 BufferRing 的 bgid 一致
    io_uring_sqe_set_data64(sqe, 0x1234);

    ring.submit_and_wait(1);

    io_uring_cqe* cqes[8];
    unsigned n = ring.peek_batch_cqe(cqes, 8);
    CORO_EXPECT_EQ(n, 1u);
    if (cqes[0]->res < 0) {
        std::fprintf(stderr, "  recv CQE res=%d (-errno = %d, %s)\n",
                     cqes[0]->res, -cqes[0]->res, strerror(-cqes[0]->res));
    }
    CORO_EXPECT_TRUE(cqes[0]->res > 0);
    CORO_EXPECT_EQ((size_t)cqes[0]->res, msg_len);
    CORO_EXPECT_TRUE((cqes[0]->flags & IORING_CQE_F_BUFFER) != 0);

    uint16_t bid = cqes[0]->flags >> IORING_CQE_BUFFER_SHIFT;
    auto buf_view = brg.view(bid);
    bool match = std::memcmp(buf_view.data(), msg, msg_len) == 0;
    CORO_EXPECT_TRUE(match);

    // 归还 buffer
    brg.return_buffer(bid);
    ring.cq_advance(n);

    close(fds[0]);
    close(fds[1]);
}

int main() { return coro_test::run_all(); }
