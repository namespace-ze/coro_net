// =============================================================================
// coro_net/tcp_connection.hpp —— 单连接的协程接口
// =============================================================================
//
// 【线程亲和】
//   一个 TcpConnection 整个生命周期内只属于一个 Scheduler（worker）。
//   它的 fd 上所有 IO（recv/send/shutdown）都通过该 Scheduler 的 io_uring 提交。
//   所以 TcpConnection 的成员函数 *只能* 在它所属的 Scheduler 线程上调用。
//
// 【shared_ptr 与所有权】
//   handler 协程的协程帧持有 shared_ptr<TcpConnection>，
//   IdleEntry 持有 weak_ptr，时间轮 bucket 持有 shared_ptr<IdleEntry>。
//   只要 handler 没结束，TcpConnection 就活着；handler co_return 后，
//   shared 计数归零，~TcpConnection 关闭 fd。
// =============================================================================

#pragma once

#include "coro_net/task.hpp"
#include "coro_net/buffer.hpp"
#include "coro_net/inet_address.hpp"
#include "coro_net/scheduler.hpp"
#include "coro_net/idle_entry.hpp"

#include <any>
#include <memory>
#include <span>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace coro_net {

class IdleConnectionWheel;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    // recv_fixed 的返回视图：n 字节数据位于 data（指向本连接的固定 slot）。
    //   n  > 0：收到的字节数
    //   n == 0：对端关闭
    //   n  < 0：-errno
    struct RecvView {
        ssize_t n = 0;
        char*   data = nullptr;
    };

    // 构造时若所属 Scheduler 有可用固定缓冲池，则借一个 slot 作为本连接
    // 整个生命周期的零拷贝读写缓冲；池满/未启用则 has_fixed_slot()==false，
    // 调用方回退到堆 Buffer 版 recv/send。析构时归还 slot 并关闭 fd。
    TcpConnection(int fd, InetAddress peer, Scheduler& sched);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    // -------------------------------------------------------------------------
    // recv —— 单次异步读
    // -------------------------------------------------------------------------
    // 返回:
    //   > 0: 实际读到的字节（已 append 到 buf 末尾）
    //   = 0: 对端关闭
    //   < 0: -errno
    //
    // 内部由 io_uring 直接写入用户 Buffer（详见 RecvIntoBufferAwaiter），
    // 同时如果 idle_entry_ 还活着，调用 wheel->refresh 续命。
    // -------------------------------------------------------------------------
    Task<ssize_t> recv(Buffer& buf);

    // -------------------------------------------------------------------------
    // recv_fixed —— 零拷贝单次读（read_fixed 进本连接的固定 slot）
    // -------------------------------------------------------------------------
    // 仅当 has_fixed_slot() 为真时可用。返回的 data 指向 slot，下次 recv_fixed
    // 会覆盖它——需在下一次读前用完（echo 场景：读完立刻 send_fixed 回去）。
    Task<RecvView> recv_fixed();

    // -------------------------------------------------------------------------
    // send_fixed —— 零拷贝写完全部字节（data 必须落在本连接的固定 slot 内）
    // -------------------------------------------------------------------------
    Task<ssize_t> send_fixed(const char* data, size_t len);

    bool has_fixed_slot() const noexcept { return buf_index_ >= 0; }

    // -------------------------------------------------------------------------
    // send —— 写完全部字节（内部循环 send，直到全发或出错）
    // -------------------------------------------------------------------------
    Task<ssize_t> send(std::span<const char> data);

    // 便捷重载
    Task<ssize_t> send(const std::string& s) {
        return send(std::span<const char>(s.data(), s.size()));
    }

    // -------------------------------------------------------------------------
    // shutdown —— 半关闭写端（让对端 recv 收到 EOF）
    // -------------------------------------------------------------------------
    Task<void> shutdown();

    int fd() const noexcept { return fd_; }
    const InetAddress& peer() const noexcept { return peer_; }
    Scheduler& scheduler() noexcept { return *sched_; }

    void set_context(std::any c) { ctx_ = std::move(c); }
    std::any& context() noexcept { return ctx_; }

    // 由 TcpServer 在 connection 建立后调用，挂上时间轮
    void install_idle(IdleConnectionWheel* w, std::shared_ptr<IdleEntry> e) {
        wheel_ = w;
        idle_entry_ = e;   // 存为 weak_ptr
    }

private:
    int fd_;
    InetAddress peer_;
    Scheduler* sched_;
    std::any ctx_;
    IdleConnectionWheel* wheel_ = nullptr;
    std::weak_ptr<IdleEntry> idle_entry_;

    // 固定注册缓冲 slot（buf_index_ < 0 表示未借到 → 走堆 Buffer 回退）
    int    buf_index_ = -1;
    char*  slot_ = nullptr;
    size_t slot_size_ = 0;
};

}  // namespace coro_net
