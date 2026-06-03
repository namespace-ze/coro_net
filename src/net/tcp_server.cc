// =============================================================================
// tcp_server.cc — TcpServer 实现
// =============================================================================
// 监听 + accept 分发：worker[0] 跑 accept 循环，其余 worker 处理连接。
// 每个 worker 各自挂一个 IdleConnectionWheel（在 tcp_server.cc 中构造，
// 但 tick_coro 必须 spawn 到 worker 线程上）。
// =============================================================================
#include "coro_net/tcp.hpp"
#include "coro_net/ops.hpp"
#include "coro_net/log.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace coro_net {

TcpServer::TcpServer(InetAddress addr, size_t worker_threads)
    : addr_(addr), worker_threads_(worker_threads == 0 ? 1 : worker_threads) {}

TcpServer::~TcpServer() {
    if (running_.load()) {
        stop();
        wait();
    }
    for (int fd : listen_fds_) {
        if (fd >= 0) ::close(fd);
    }
    listen_fds_.clear();
}

static int make_listen_socket(const InetAddress& addr) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::system_error(errno, std::system_category(), "socket");
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    if (::bind(fd, (const sockaddr*)&addr.raw(), sizeof(sockaddr_in)) < 0) {
        int e = errno;
        ::close(fd);
        throw std::system_error(e, std::system_category(), "bind");
    }
    if (::listen(fd, 4096) < 0) {
        int e = errno;
        ::close(fd);
        throw std::system_error(e, std::system_category(), "listen");
    }
    return fd;
}

void TcpServer::start() {
    if (running_.exchange(true)) return;

    // write_fixed（固定缓冲发送）不带 MSG_NOSIGNAL，对端断开会触发 SIGPIPE；
    // 服务器统一忽略之（普通 send 走 MSG_NOSIGNAL，此处也无害）。
    ::signal(SIGPIPE, SIG_IGN);

    // 按调优参数构造 SchedulerPool（每 Scheduler 各自建 ring + 可选固定缓冲池）。
    SchedulerConfig cfg;
    cfg.sqpoll = (sqpoll_threads_ > 0);
    cfg.use_fixed_buffers = use_fixed_buffers_;
    cfg.buf_slot_size = buf_slot_size_;
    cfg.buf_pool_capacity =
        use_fixed_buffers_
            ? (buf_pool_capacity_ ? buf_pool_capacity_ : kDefaultBufCapacity)
            : 0;
    pool_ = std::make_unique<SchedulerPool>(worker_threads_, cfg, sqpoll_threads_,
                                            sqpoll_cpus_);

    const size_t n = pool_->size();

    // 每个 worker 一个 SO_REUSEPORT listen fd（同端口，内核按四元组哈希分流）。
    listen_fds_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        listen_fds_[i] = make_listen_socket(addr_);
    }
    LOG_INFO << "TcpServer listening on " << addr_.to_string()
             << " workers=" << n << " (SO_REUSEPORT, M=" << sqpoll_threads_
             << " SQPOLL)";

    // 为每个 worker 预创建时间轮（如配置了 idle 超时）+ 启动本地 accept 循环。
    wheels_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        Scheduler& s = pool_->at(i);

        if (idle_.count() > 0) {
            wheels_[i] = std::make_unique<IdleConnectionWheel>(s, idle_);
            IdleConnectionWheel* w = wheels_[i].get();
            s.queue_boot_task([w]() { w->start(); });
        }

        // 每个 worker 在 *自己的线程* 上 accept 自己的 listen fd，并就地建连、
        // 就地 spawn handler——连接不跨线程，无 post_task 派发。
        s.queue_boot_task([this, i]() {
            Scheduler& s = pool_->at(i);
            s.spawn([](TcpServer* server, Scheduler& s, size_t idx) -> Task<void> {
                int lfd = server->listen_fds_[idx];
                IdleConnectionWheel* wheel = server->wheels_[idx].get();
                while (server->running_.load()) {
                    AcceptAwaiter aw{lfd, s};
                    int conn = co_await aw;
                    if (conn < 0) break;  // -ECANCELED / -EBADF → 退出
                    InetAddress peer{aw.peer()};
                    auto tc = std::make_shared<TcpConnection>(conn, peer, s);
                    if (wheel) {
                        auto entry = wheel->register_conn(tc);
                        tc->install_idle(wheel, entry);
                    }
                    // handler_ 按指针引用同一份 closure（生命周期与 server 一致）。
                    s.spawn(server->handler_(std::move(tc)));
                }
                co_return;
            }(this, s, i));
        });
    }

    pool_->start();
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO << "TcpServer stopping";

    // 关闭所有 listen fd → 让各 worker 的 AcceptAwaiter 收到 -ECANCELED/-EBADF 退出
    for (int fd : listen_fds_) {
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
    }
    // 通知所有 worker 停止（跨线程统一走 eventfd 唤醒，见 Scheduler::stop）
    if (pool_) {
        for (size_t i = 0; i < pool_->size(); ++i) {
            Scheduler* w = &pool_->at(i);
            w->post_task([w]() { w->stop(); });
        }
    }
}

void TcpServer::wait() {
    if (pool_) pool_->wait();
}

}  // namespace coro_net
