// =============================================================================
// async_logger.cc —— 双缓冲后端实现
// =============================================================================
#include "coro_net/log/async_logger.hpp"
#include "coro_net/log/log_file.hpp"

#include <pthread.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace coro_net {

// -----------------------------------------------------------------------------
// 单例：Meyers 风格，第一次访问构造，进程结束最后析构
// -----------------------------------------------------------------------------
AsyncLogger& AsyncLogger::instance() noexcept {
    static AsyncLogger inst;
    return inst;
}

AsyncLogger::~AsyncLogger() {
    terminating_.store(true, std::memory_order_release);
    if (started_.load(std::memory_order_acquire)) {
        running_.store(false, std::memory_order_release);
        cond_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
}

// -----------------------------------------------------------------------------
// init / shutdown / start_locked
// -----------------------------------------------------------------------------
void AsyncLogger::init(std::string basename, off_t roll_size, int flush_interval_seconds) {
    auto& self = instance();
    std::lock_guard<std::mutex> lk(self.mutex_);
    if (self.started_.load(std::memory_order_acquire)) return;
    self.basename_       = std::move(basename);
    self.roll_size_      = roll_size;
    self.flush_interval_ = flush_interval_seconds;
    self.start_locked();
}

void AsyncLogger::shutdown() noexcept {
    auto& self = instance();
    bool was = self.running_.exchange(false, std::memory_order_acq_rel);
    if (!was) return;
    self.cond_.notify_all();
    if (self.thread_.joinable()) self.thread_.join();
    self.started_.store(false, std::memory_order_release);
}

void AsyncLogger::start_locked() {
    current_ = std::make_unique<LargeBuffer>();
    next_    = std::make_unique<LargeBuffer>();
    buffers_.reserve(16);
    running_.store(true, std::memory_order_release);
    started_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { thread_func(); });
}

// -----------------------------------------------------------------------------
// append —— 前端入口（任意线程）
// -----------------------------------------------------------------------------
void AsyncLogger::append(const char* logline, int len) noexcept {
    if (terminating_.load(std::memory_order_acquire)) {
        fallback_stderr(logline, len);
        return;
    }
    if (!started_.load(std::memory_order_acquire)) {
        // 未 init：直接 stderr，便于测试 / 早期诊断
        fallback_stderr(logline, len);
        return;
    }

    std::lock_guard<std::mutex> lk(mutex_);
    if (current_->avail() > len) {
        current_->append(logline, static_cast<size_t>(len));
        return;
    }
    // 当前 buffer 装不下：把它推到 buffers_，换上 next_（或新分配）
    buffers_.push_back(std::move(current_));
    if (next_) {
        current_ = std::move(next_);
    } else {
        current_ = std::make_unique<LargeBuffer>();
    }
    current_->append(logline, static_cast<size_t>(len));
    cond_.notify_one();
}

void AsyncLogger::fallback_stderr(const char* logline, int len) noexcept {
    ::fwrite(logline, 1, static_cast<size_t>(len), stderr);
}

// -----------------------------------------------------------------------------
// thread_func —— 后端线程
// -----------------------------------------------------------------------------
void AsyncLogger::thread_func() {
    ::pthread_setname_np(::pthread_self(), "coro_log_bg");

    LogFile output(basename_, roll_size_, flush_interval_);

    BufferPtr new_buf1 = std::make_unique<LargeBuffer>();
    BufferPtr new_buf2 = std::make_unique<LargeBuffer>();
    BufferVec buffers_to_write;
    buffers_to_write.reserve(16);

    while (running_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(mutex_);
            if (buffers_.empty()) {
                cond_.wait_for(lk, std::chrono::seconds(flush_interval_));
            }
            // 强制把 current_ 也推过去（即便没满）—— 避免最后一秒的日志卡住
            buffers_.push_back(std::move(current_));
            current_ = std::move(new_buf1);
            buffers_to_write.swap(buffers_);
            if (!next_) {
                next_ = std::move(new_buf2);
            }
        }

        // 超过 25 个 4MB buffer ≈ 100MB 没刷出去 → 丢弃多余，避免内存爆
        if (buffers_to_write.size() > 25) {
            char warn[200];
            int n = std::snprintf(warn, sizeof warn,
                "Dropped %zu log buffers (over backpressure limit)\n",
                buffers_to_write.size() - 2);
            if (n > 0) output.append(warn, n);
            buffers_to_write.erase(buffers_to_write.begin() + 2,
                                   buffers_to_write.end());
        }

        for (auto& buf : buffers_to_write) {
            output.append(buf->data(), buf->length());
        }

        if (buffers_to_write.size() > 2) {
            buffers_to_write.resize(2);
        }
        if (!new_buf1) {
            new_buf1 = std::move(buffers_to_write.back());
            buffers_to_write.pop_back();
            new_buf1->reset();
        }
        if (!new_buf2 && !buffers_to_write.empty()) {
            new_buf2 = std::move(buffers_to_write.back());
            buffers_to_write.pop_back();
            new_buf2->reset();
        }
        buffers_to_write.clear();
        output.flush();
    }

    // 收尾：把残留的 buffers_ 一次性刷掉
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (current_ && current_->length() > 0) {
            buffers_.push_back(std::move(current_));
        }
    }
    for (auto& buf : buffers_) {
        if (buf) output.append(buf->data(), buf->length());
    }
    buffers_.clear();
    output.flush();
}

void AsyncLogger::flush_and_stop() noexcept {
    if (!started_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    cond_.notify_all();
    if (thread_.joinable()) thread_.join();
    started_.store(false, std::memory_order_release);
}

}  // namespace coro_net
