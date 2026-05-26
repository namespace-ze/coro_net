// =============================================================================
// coro_net/log/async_logger.hpp —— 双缓冲异步后端（muduo 风格）
// =============================================================================
// 前端 append(): 持锁 memcpy 到 current_，满则换 next_，触发后端线程刷盘。
// 后端线程：cond_.wait_for(3s) 或 buffers_ 非空时醒来，swap 出来后释放锁，
//          串行写到 LogFile。
//
// 单例：未调 init() 时 lazy fallback 到 stderr，便于测试代码不强制初始化。
// =============================================================================

#pragma once

#include "coro_net/log/log_stream.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/types.h>

namespace coro_net {

class LogFile;

class AsyncLogger {
public:
    // 显式初始化：设置文件 basename / 滚动大小 / 刷盘周期；并启动后端线程
    static void init(std::string basename,
                     off_t roll_size = 64 * 1024 * 1024,
                     int flush_interval_seconds = 3);
    // 终止后端线程（最后刷盘）；可重入
    static void shutdown() noexcept;

    static AsyncLogger& instance() noexcept;

    // 由 Logger 析构调用：把一行日志（含换行）扔到 backend
    void append(const char* logline, int len) noexcept;

    // 强制刷盘并停止后端线程（LOG_FATAL 用）
    void flush_and_stop() noexcept;

    bool started() const noexcept { return started_.load(std::memory_order_acquire); }

private:
    AsyncLogger() = default;
    ~AsyncLogger();

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void start_locked();
    void thread_func();
    void fallback_stderr(const char* logline, int len) noexcept;

    using BufferPtr = std::unique_ptr<LargeBuffer>;
    using BufferVec = std::vector<BufferPtr>;

    std::mutex mutex_;
    std::condition_variable cond_;
    BufferPtr current_;
    BufferPtr next_;
    BufferVec buffers_;

    std::thread thread_;
    std::atomic<bool> started_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> terminating_{false};

    // init 之前的临时状态
    std::string basename_;
    off_t       roll_size_ = 64 * 1024 * 1024;
    int         flush_interval_ = 3;
};

}  // namespace coro_net
