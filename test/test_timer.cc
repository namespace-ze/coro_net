// =============================================================================
// test_timer.cc —— TimerQueue / Scheduler::run_after / run_every / cancel
// =============================================================================
#include "coro_net/scheduler.hpp"
#include "coro_net/timer/timer_id.hpp"
#include "coro_net/timer/timer_queue.hpp"
#include "coro_net/log.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using coro_net::Scheduler;
using coro_net::TimerId;

// -----------------------------------------------------------------------------
// 1. 一次性 timer 触发后 stop()
// -----------------------------------------------------------------------------
CORO_TEST(timer_one_shot_fires_once) {
    Scheduler sch;
    std::atomic<int> counter{0};
    auto t0 = std::chrono::steady_clock::now();

    std::thread th([&] {
        sch.run();
    });
    // 主线程注入一个 boot task：run() 启动后调度一次性 timer
    sch.post_task([&] {
        Scheduler::current()->run_after(20ms, [&] {
            counter.fetch_add(1);
            Scheduler::current()->stop();
        });
    });
    // 实际触发依赖 run_after 在 worker 线程里调度——上面是 post_task 经
    // eventfd 唤醒投递到 worker，run_after 内部判 tls_current_==this 走同线程路径
    th.join();
    auto dt = std::chrono::steady_clock::now() - t0;

    CORO_EXPECT_EQ(counter.load(), 1);
    CORO_EXPECT_TRUE(dt >= 20ms);
    CORO_EXPECT_TRUE(dt < 500ms);
}

// -----------------------------------------------------------------------------
// 2. 重复 timer 触发 5 次后停止
// -----------------------------------------------------------------------------
CORO_TEST(timer_repeating_fires_multiple) {
    Scheduler sch;
    std::atomic<int> counter{0};

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        Scheduler::current()->run_every(10ms, [&] {
            int v = counter.fetch_add(1) + 1;
            if (v >= 5) Scheduler::current()->stop();
        });
    });
    th.join();
    CORO_EXPECT_TRUE(counter.load() >= 5);
}

// -----------------------------------------------------------------------------
// 3. 调度后立即 cancel：回调不应该被调用
// -----------------------------------------------------------------------------
CORO_TEST(timer_cancel_before_fire) {
    Scheduler sch;
    std::atomic<int> bad_counter{0};

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        auto* s = Scheduler::current();
        TimerId id = s->run_after(50ms, [&] {
            bad_counter.fetch_add(1);
        });
        s->cancel(id);
        // 80ms 后 stop（给被 cancel 的 50ms timer 充分时间证伪）
        s->run_after(80ms, [] {
            Scheduler::current()->stop();
        });
    });
    th.join();
    CORO_EXPECT_EQ(bad_counter.load(), 0);
}

// -----------------------------------------------------------------------------
// 4. 在回调内部 cancel 自己（重复 timer）
// -----------------------------------------------------------------------------
CORO_TEST(timer_cancel_repeating_inside_callback) {
    Scheduler sch;
    std::atomic<int> counter{0};

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        auto* s = Scheduler::current();
        // 把 id 用 shared_ptr 包起来让 lambda 捕到 stable storage
        auto id_holder = std::make_shared<TimerId>();
        *id_holder = s->run_every(10ms, [&, id_holder] {
            int v = counter.fetch_add(1) + 1;
            if (v == 3) {
                Scheduler::current()->cancel(*id_holder);
            }
        });
        s->run_after(80ms, [&] { Scheduler::current()->stop(); });
    });
    th.join();
    // 应触发恰好 3 次（cancel 后不再增加）
    CORO_EXPECT_EQ(counter.load(), 3);
}

// -----------------------------------------------------------------------------
// 5. 多个交错 deadline 按时序触发
// -----------------------------------------------------------------------------
CORO_TEST(timer_multiple_independent) {
    Scheduler sch;
    std::vector<int> order;
    std::mutex order_mu;

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        auto* s = Scheduler::current();
        for (int i = 1; i <= 5; ++i) {
            s->run_after(std::chrono::milliseconds(10 * i), [i, &order, &order_mu] {
                std::lock_guard<std::mutex> lk(order_mu);
                order.push_back(i);
            });
        }
        s->run_after(80ms, [&] { Scheduler::current()->stop(); });
    });
    th.join();
    CORO_EXPECT_EQ(order.size(), (size_t)5);
    for (size_t i = 0; i < order.size(); ++i) {
        CORO_EXPECT_EQ(order[i], (int)(i + 1));
    }
}

// -----------------------------------------------------------------------------
// 6. 零延迟 timer：必须立即触发（防止 timerfd_settime {0,0} → disarm 陷阱）
// -----------------------------------------------------------------------------
CORO_TEST(timer_zero_delay) {
    Scheduler sch;
    std::atomic<bool> fired{false};

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        auto* s = Scheduler::current();
        s->run_after(0ns, [&] {
            fired.store(true);
            Scheduler::current()->stop();
        });
    });
    th.join();
    CORO_EXPECT_TRUE(fired.load());
}

// -----------------------------------------------------------------------------
// 7. callback 抛异常不应该让事件循环挂掉
// -----------------------------------------------------------------------------
CORO_TEST(timer_callback_exception_does_not_kill_loop) {
    Scheduler sch;
    std::atomic<int> good_counter{0};

    std::thread th([&] { sch.run(); });
    sch.post_task([&] {
        auto* s = Scheduler::current();
        s->run_after(10ms, [] {
            throw std::runtime_error("boom");
        });
        s->run_after(30ms, [&] {
            good_counter.fetch_add(1);
            Scheduler::current()->stop();
        });
    });
    th.join();
    CORO_EXPECT_EQ(good_counter.load(), 1);
}

int main() {
    // 测试期间日志降到 ERROR，避免 DEBUG/INFO 干扰
    coro_net::Logger::set_global_level(coro_net::LogLevel::ERROR);
    return coro_test::run_all();
}
