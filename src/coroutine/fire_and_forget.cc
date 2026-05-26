// =============================================================================
// fire_and_forget.cc —— FireAndForget::promise_type::unhandled_exception 的
// 非内联实现：通过 Logger 写一行 FATAL，然后 abort（保留 coredump）。
// =============================================================================
#include "coro_net/fire_and_forget.hpp"
#include "coro_net/log.hpp"

#include <cstdlib>
#include <exception>
#include <stdexcept>

namespace coro_net::detail {

void log_fire_and_forget_uncaught() noexcept {
    try {
        std::exception_ptr ep = std::current_exception();
        if (ep) std::rethrow_exception(ep);
    } catch (const std::exception& e) {
        LOG_FATAL << "FireAndForget uncaught exception: " << e.what();
    } catch (...) {
        LOG_FATAL << "FireAndForget uncaught exception: (unknown)";
    }
    // LOG_FATAL 的 ~Logger 已经 abort 过；保险起见再终止一次
    std::terminate();
}

}  // namespace coro_net::detail
