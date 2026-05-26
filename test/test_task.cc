// =============================================================================
// test_task.cc — Task<T> 与 FireAndForget 单元测试
// =============================================================================
// 验证：
//   1. Task<int> 基本：co_return / await_resume 拿到值
//   2. Task<void> 基本：能正常 co_return; (没有值)
//   3. 嵌套 await 不爆栈（symmetric transfer 验证）
//   4. 异常透传：协程内抛出异常，外层 co_await 能 catch
//   5. FireAndForget 立刻执行，不需要外部 resume
// =============================================================================

#include "coro_net/task.hpp"
#include "coro_net/fire_and_forget.hpp"
#include "test_util.hpp"

#include <stdexcept>
#include <string>

using coro_net::Task;
using coro_net::FireAndForget;

// -----------------------------------------------------------------------------
// 工具：手动 driver — 因为 Task 是 lazy，需要一个"假装事件循环"的驱动来跑它
// -----------------------------------------------------------------------------
//
// 真实 Scheduler 在 S3 实现。这里我们手工模拟：
//   1) 拿到 Task 后，调用 task.handle().resume() 启动协程
//   2) 协程一路跑到下一个 co_await（或 co_return）后挂起
//   3) 因为我们的测试用例里 co_await 的对象都是其他 Task（同步立即完成），
//      所以 symmetric transfer 会自动把控制权链式传下去，最终回到顶层
//   4) 最终顶层 Task 的 handle.done() 为 true，可以读 result
// -----------------------------------------------------------------------------
template <typename T>
T sync_wait(Task<T>&& task) {
    auto h = task.handle();
    h.resume();  // 启动 lazy 任务；symmetric transfer 会把整个链条跑完
    // 此时顶层协程应该已经完成（因为子任务都是同步的）
    if (!h.done()) {
        std::fprintf(stderr, "  sync_wait: task did not complete synchronously!\n");
        std::abort();
    }
    if constexpr (std::is_same_v<T, void>) {
        // 检查异常并 rethrow
        if (h.promise().exception_) std::rethrow_exception(h.promise().exception_);
        return;
    } else {
        auto& result = h.promise().result_;
        if (result.index() == 2) std::rethrow_exception(std::get<2>(result));
        return std::move(std::get<1>(result));
    }
}

// -----------------------------------------------------------------------------
// 1. Task<int> 基本
// -----------------------------------------------------------------------------
static Task<int> answer() {
    co_return 42;
}

CORO_TEST(task_int_basic) {
    int r = sync_wait(answer());
    CORO_EXPECT_EQ(r, 42);
}

// -----------------------------------------------------------------------------
// 2. Task<void> 基本
// -----------------------------------------------------------------------------
static int side_effect = 0;

static Task<void> do_void() {
    side_effect = 7;
    co_return;
}

CORO_TEST(task_void_basic) {
    side_effect = 0;
    sync_wait(do_void());
    CORO_EXPECT_EQ(side_effect, 7);
}

// -----------------------------------------------------------------------------
// 3. 嵌套 await 不爆栈
// -----------------------------------------------------------------------------
//
// 这里递归 1000 层 co_await，如果 final_suspend 不是 symmetric transfer，
// 每层会增加一帧栈深，1000 层基本就爆栈了。symmetric transfer 让"协程结束 →
// 跳回外层"是一个 tail call，栈深保持常量。
// -----------------------------------------------------------------------------
static Task<int> recursive(int n) {
    if (n == 0) co_return 0;
    int sub = co_await recursive(n - 1);
    co_return sub + 1;
}

CORO_TEST(task_deep_nested_no_stack_overflow) {
    int r = sync_wait(recursive(1000));
    CORO_EXPECT_EQ(r, 1000);
}

// -----------------------------------------------------------------------------
// 4. 异常透传
// -----------------------------------------------------------------------------
static Task<int> thrower() {
    throw std::runtime_error("boom");
    co_return 0;  // 永远到不了，但要让编译器知道这是协程
}

static Task<int> caller_of_thrower() {
    int v = co_await thrower();   // 这里会重新抛出
    co_return v + 1;
}

CORO_TEST(task_exception_propagates_through_await) {
    bool caught = false;
    try {
        sync_wait(caller_of_thrower());
    } catch (const std::runtime_error& e) {
        caught = (std::string(e.what()) == "boom");
    }
    CORO_EXPECT_TRUE(caught);
}

// -----------------------------------------------------------------------------
// 5. FireAndForget 立即执行
// -----------------------------------------------------------------------------
static int faf_ran = 0;

static FireAndForget faf_task() {
    faf_ran = 1;
    co_return;
}

CORO_TEST(fire_and_forget_runs_eagerly) {
    faf_ran = 0;
    faf_task();                         // 直接调用，不需要 resume / 不需要等待
    CORO_EXPECT_EQ(faf_ran, 1);
}

// -----------------------------------------------------------------------------
// 6. Task move-only 语义
// -----------------------------------------------------------------------------
CORO_TEST(task_is_move_only) {
    Task<int> a = answer();
    Task<int> b = std::move(a);          // move 构造
    CORO_EXPECT_EQ(sync_wait(std::move(b)), 42);
    // a 已被掏空，不应再使用（其 handle 是空）
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------
int main() {
    return coro_test::run_all();
}
