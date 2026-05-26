// =============================================================================
// test_io_uring.cc — IoUring 基本功能测试
// =============================================================================
// 验证：
//   1. IoUring 能构造、提交 NOP、收到 CQE
//   2. 直接给 io_uring 一个用户 buffer 收到 socketpair 写入数据
//      （已用 per-conn Buffer 路径取代 BufferRing，详见 plan 精简 B）
// =============================================================================

#include "coro_net/io/io_uring.h"
#include "test_util.hpp"

#include <liburing.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cstring>

using coro_net::IoUring;

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
// 2. 直接 recv 到用户 buffer（不走 BufferRing）
// -----------------------------------------------------------------------------
CORO_TEST(iouring_recv_into_user_buf) {
    IoUring ring(64);

    int fds[2];
    CORO_EXPECT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    const char* msg = "hello io_uring";
    size_t msg_len = std::strlen(msg);
    ssize_t w = write(fds[1], msg, msg_len);
    CORO_EXPECT_EQ((size_t)w, msg_len);

    char buf[256] = {0};
    io_uring_sqe* sqe = ring.get_sqe();
    CORO_EXPECT_TRUE(sqe != nullptr);
    io_uring_prep_recv(sqe, fds[0], buf, sizeof buf, 0);
    io_uring_sqe_set_data64(sqe, 0x1234);

    ring.submit_and_wait(1);

    io_uring_cqe* cqes[8];
    unsigned n = ring.peek_batch_cqe(cqes, 8);
    CORO_EXPECT_EQ(n, 1u);
    CORO_EXPECT_TRUE(cqes[0]->res > 0);
    CORO_EXPECT_EQ((size_t)cqes[0]->res, msg_len);
    CORO_EXPECT_TRUE(std::memcmp(buf, msg, msg_len) == 0);
    ring.cq_advance(n);

    close(fds[0]);
    close(fds[1]);
}

int main() { return coro_test::run_all(); }
