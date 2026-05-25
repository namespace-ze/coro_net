// =============================================================================
// thread_pool/coro_thread_pool.cc —— CoroThreadPool 实现
// =============================================================================
#include "coro_net/thread_pool.hpp"

#include <cstdio>

namespace coro_net {

CoroThreadPool::CoroThreadPool(std::string name, size_t threads)
    : name_(std::move(name)), target_threads_(threads == 0 ? 1 : threads) {}

CoroThreadPool::~CoroThreadPool() {
    if (started_) {
        stop();
    }
}

void CoroThreadPool::start() {
    if (started_) return;
    started_ = true;
    stopping_.store(false);
    threads_.reserve(target_threads_);
    for (size_t i = 0; i < target_threads_; ++i) {
        threads_.emplace_back([this] { worker_loop(); });
    }
}

void CoroThreadPool::stop() {
    if (!started_) return;
    stopping_.store(true);
    cv_.notify_all();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    started_ = false;
}

void CoroThreadPool::enqueue(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push(std::move(task));
    }
    cv_.notify_one();
}

void CoroThreadPool::worker_loop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return !queue_.empty() || stopping_.load();
            });
            if (stopping_.load() && queue_.empty()) return;
            task = std::move(queue_.front());
            queue_.pop();
        }
        try {
            task();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[%s] worker uncaught: %s\n",
                         name_.c_str(), e.what());
        } catch (...) {
            std::fprintf(stderr, "[%s] worker uncaught: unknown\n",
                         name_.c_str());
        }
    }
}

}  // namespace coro_net
