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
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace coro_net {

TcpServer::TcpServer(InetAddress addr, size_t worker_threads)
    : addr_(addr), pool_(worker_threads == 0 ? 1 : worker_threads) {
    if (worker_threads == 0) worker_threads = 1;
}

TcpServer::~TcpServer() {
    if (running_.load()) {
        stop();
        wait();
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
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

    listen_fd_ = make_listen_socket(addr_);
    LOG_INFO << "TcpServer listening fd=" << listen_fd_
             << " workers=" << pool_.size();

    // 为每个 worker 预创建并启动时间轮（如果配置了 idle 超时）
    wheels_.resize(pool_.size());
    for (size_t i = 0; i < pool_.size(); ++i) {
        if (idle_.count() > 0) {
            // 时间轮对象在主线程构造，但其 tick_coro 会在 worker 线程 spawn。
            // start() 必须在 worker 线程调用——所以放进 queue_boot_task。
            wheels_[i] = std::make_unique<IdleConnectionWheel>(pool_.at(i), idle_);
            IdleConnectionWheel* w = wheels_[i].get();
            pool_.at(i).queue_boot_task([w]() {
                w->start();
            });
        }
    }

    // worker[0] 跑 accept 循环
    Scheduler* accept_sched = &pool_.at(0);
    pool_.at(0).queue_boot_task([this, accept_sched]() {
        accept_sched->spawn([](TcpServer* server, Scheduler& s) -> Task<void> {
            while (server->running_.load()) {
                AcceptAwaiter aw{server->listen_fd_, s};
                int conn = co_await aw;
                if (conn < 0) {
                    // -ECANCELED / 其它错误 → 退出 accept 循环
                    break;
                }
                InetAddress peer{aw.peer()};
                // round-robin 选下一个 worker（这里包含 worker[0] 自己也可能被选中）
                size_t idx = server->pool_.size() == 1 ? 0
                            : (1 + (size_t)conn % (server->pool_.size() - 1));
                // 当 size==1 时所有都在 0；否则避开 0
                if (server->pool_.size() > 1) {
                    // 用 fd 做 hash 避免 round-robin 偏置, 待优化
                }
                Scheduler* target = &server->pool_.at(idx);
                IdleConnectionWheel* wheel = server->wheels_[idx].get();
                TcpServer* srv = server;

                // 关键：不要把 handler_ 按值拷贝到 post_task lambda 的 captures
                // 里——那会创建一份临时 std::function 副本，lambda 退出时被析构，
                // 但 lambda 内部已经 spawn 的协程帧仍持有指向该副本中
                // *closure 对象* 的 this 指针（lambda-as-coroutine 的 implicit
                // first arg），从而 use-after-free。
                // 正确做法：仅按指针引用 srv->handler_，每次调用都是同一份
                // closure，生命周期与 server 一致。
                target->post_task([srv, conn, peer, target, wheel]() {
                    auto tc = std::make_shared<TcpConnection>(conn, peer, *target);
                    if (wheel) {
                        auto entry = wheel->register_conn(tc);
                        tc->install_idle(wheel, entry);
                    }
                    Scheduler::current()->spawn(srv->handler_(std::move(tc)));
                });
            }
            co_return;
        }(this, *accept_sched));
    });

    pool_.start();
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;
    LOG_INFO << "TcpServer stopping";

    // 关闭 listen fd → 让 accept_awaiter 收到 -ECANCELED 或 -EBADF，退出循环
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
    }
    // 通知所有 worker 停止：每个 worker 自己停自己，跨线程 wake 需 ring，
    // 这里通过 post_task 投递（来源若没有 ring 会失败，但 boot tasks 配合
    // 一个守护协程会兜底——简化版只做尽力而为）
    for (size_t i = 0; i < pool_.size(); ++i) {
        Scheduler* w = &pool_.at(i);
        w->post_task([w]() { w->stop(); });
    }
}

void TcpServer::wait() {
    pool_.wait();
}

}  // namespace coro_net
