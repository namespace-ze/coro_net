// =============================================================================
// net/tcp.cc — TcpConnection / IdleConnectionWheel / TcpServer 实现
// =============================================================================
#include "coro_net/tcp.hpp"
#include "coro_net/ops.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace coro_net {

// =============================================================================
// IdleEntry 析构 —— 时间轮把这个 entry 完全淘汰时（所有桶都没有引用），
// 触发关闭对应连接的协程
// =============================================================================
IdleEntry::~IdleEntry() {
    auto c = wconn.lock();
    if (!c || !sched) return;

    // spawn 关闭协程：必须在 sched 所属的 worker 线程上运行；
    // 但 ~IdleEntry 是从 buckets_.push_back 触发的，调用方是 wheel.tick_coro
    // ——已经在 sched 线程上了。所以可以直接 spawn。
    //
    // 不能直接 c->shutdown()，因为 shutdown 是 Task<void> 需要 await；
    // 这里我们在析构里没法 co_await。
    sched->spawn([](TcpConnectionPtr c_) -> Task<void> {
        co_await c_->shutdown();
        co_return;
    }(c));
}

// =============================================================================
// TcpConnection 实现
// =============================================================================
Task<ssize_t> TcpConnection::recv(Buffer& buf) {
    ssize_t n = co_await RecvIntoBufferAwaiter{fd_, buf, *sched_};
    // 续命：每次有数据进来就把自己重新插入队尾桶
    if (n > 0 && wheel_) {
        if (auto e = idle_entry_.lock()) {
            wheel_->refresh(e);
        }
    }
    co_return n;
}

Task<ssize_t> TcpConnection::send(std::span<const char> data) {
    size_t remaining = data.size();
    const char* p = data.data();
    ssize_t total = 0;
    while (remaining > 0) {
        ssize_t n = co_await SendAwaiter{fd_, p, remaining, *sched_};
        if (n < 0) co_return n;
        if (n == 0) co_return total;
        total += n;
        p += n;
        remaining -= n;
    }
    co_return total;
}

Task<void> TcpConnection::shutdown() {
    if (fd_ < 0) co_return;
    co_await ShutdownAwaiter{fd_, SHUT_RDWR, *sched_};
    co_return;
}

// =============================================================================
// IdleConnectionWheel 实现
// =============================================================================
IdleConnectionWheel::IdleConnectionWheel(Scheduler& s, std::chrono::seconds idle)
    : sched_(&s), buckets_(idle.count() > 0 ? (size_t)idle.count() : 1) {
    // 预填 N 个空桶
    size_t N = idle.count() > 0 ? (size_t)idle.count() : 1;
    for (size_t i = 0; i < N; ++i) {
        buckets_.push_back(Bucket{});
    }
}

void IdleConnectionWheel::start() {
    running_.store(true, std::memory_order_relaxed);
    sched_->spawn(tick_coro());
}

std::shared_ptr<IdleEntry> IdleConnectionWheel::register_conn(
    std::weak_ptr<TcpConnection> c) {
    // 注意：使用显式构造函数，避免 IdleEntry{...} 聚合初始化产生临时对象，
    // 该临时对象的析构会被 ~IdleEntry 误判为"超时淘汰"，从而立即关闭新连接。
    auto e = std::make_shared<IdleEntry>(std::move(c), sched_);
    buckets_.back().insert(e);
    return e;
}

void IdleConnectionWheel::refresh(const std::shared_ptr<IdleEntry>& e) {
    if (e) buckets_.back().insert(e);
}

Task<void> IdleConnectionWheel::tick_coro() {
    using namespace std::chrono_literals;
    while (running_.load(std::memory_order_relaxed)) {
        co_await TimeoutAwaiter{1s, *sched_};
        if (!running_.load(std::memory_order_relaxed)) break;
        // push_back 空桶：覆盖最旧桶 → 旧桶中所有 shared_ptr<IdleEntry>
        // 引用计数减一；若该 entry 不再被其它桶持有 → ~IdleEntry → 关闭连接
        buckets_.push_back(Bucket{});
    }
    co_return;
}

// =============================================================================
// TcpServer 实现
// =============================================================================
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
                    // 用 fd 做 hash 避免 round-robin 偏置（教学版简化）
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
