// =============================================================================
// coro_net/log/logger.hpp —— 前端 Logger 对象 + LOG_* 宏
// =============================================================================
// 每条日志构造一个临时 Logger 对象（栈上），<< 链写入它的 LogStream，
// 析构时把整行交给 AsyncLogger 后端。
// =============================================================================

#pragma once

#include "coro_net/log/log_level.hpp"
#include "coro_net/log/log_stream.hpp"

#include <atomic>

namespace coro_net {

class Logger {
public:
    // SourceFile：自动从 __FILE__ 抽取 basename，避免日志里满屏路径
    struct SourceFile {
        template <int N>
        SourceFile(const char (&arr)[N]) noexcept
            : data_(arr), size_(N - 1) {
            const char* slash = nullptr;
            for (int i = N - 2; i >= 0; --i) {
                if (arr[i] == '/') { slash = arr + i + 1; break; }
            }
            if (slash) {
                data_ = slash;
                size_ = static_cast<int>((arr + N - 1) - slash);
            }
        }
        explicit SourceFile(const char* filename) noexcept
            : data_(filename), size_(0) {
            const char* slash = std::strrchr(filename, '/');
            if (slash) data_ = slash + 1;
            size_ = static_cast<int>(std::strlen(data_));
        }
        const char* data_;
        int size_;
    };

    Logger(SourceFile file, int line, LogLevel level) noexcept;
    Logger(SourceFile file, int line, LogLevel level, const char* func) noexcept;
    // toAbort：兼容 LOG_SYSFATAL 等，true 时析构后 abort
    Logger(SourceFile file, int line, bool toAbort) noexcept;
    ~Logger() noexcept;

    LogStream& stream() noexcept { return stream_; }

    static LogLevel global_level() noexcept {
        return global_level_.load(std::memory_order_relaxed);
    }
    static void set_global_level(LogLevel lv) noexcept {
        global_level_.store(lv, std::memory_order_relaxed);
    }

private:
    void format_prefix(LogLevel level) noexcept;

    LogStream stream_;
    LogLevel level_;
    int line_;
    SourceFile file_;
    bool to_abort_ = false;

    static std::atomic<LogLevel> global_level_;
};

}  // namespace coro_net

// -----------------------------------------------------------------------------
// LOG_* 宏
//
// 设计要点：
//   - 编译期下限 CORO_NET_LOG_MIN_LEVEL_VAL 直接把分支编译掉（零开销）
//   - 运行期下限 Logger::global_level() 通过 if 提前短路（不构造 Logger）
//   - 由于 << 链的最左侧必须是 Logger().stream()，宏展开后是
//       if (level通过) Logger(...).stream() << "msg"
//     这里 if 没有 else，所以下游再写 if/else 不会被这条吞掉——保险起见外层 do{}while(0) 不行
//     （会切断 << 链）；我们用 "if (0) ; else if (..) Logger().stream()" 的惯例避免吞掉。
// -----------------------------------------------------------------------------

#define CORO_NET_LOG_IF(cond) \
    if (0) ; else if (cond) ::coro_net::Logger(__FILE__, __LINE__,

#define LOG_TRACE \
    if constexpr (CORO_NET_LOG_MIN_LEVEL_VAL > 0) ; \
    else if (::coro_net::Logger::global_level() <= ::coro_net::LogLevel::TRACE) \
        ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::TRACE, __func__).stream()

#define LOG_DEBUG \
    if constexpr (CORO_NET_LOG_MIN_LEVEL_VAL > 1) ; \
    else if (::coro_net::Logger::global_level() <= ::coro_net::LogLevel::DEBUG) \
        ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::DEBUG, __func__).stream()

#define LOG_INFO \
    if constexpr (CORO_NET_LOG_MIN_LEVEL_VAL > 2) ; \
    else if (::coro_net::Logger::global_level() <= ::coro_net::LogLevel::INFO) \
        ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::INFO).stream()

#define LOG_WARN  ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::WARN).stream()
#define LOG_ERROR ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::ERROR).stream()
#define LOG_FATAL ::coro_net::Logger(__FILE__, __LINE__, ::coro_net::LogLevel::FATAL).stream()

// 系统调用错误：Logger 析构时自动追加 " (errno=N: strerror)"。
// 由 logger.cc 内部通过 to_abort_ 控制 abort 与否；这里只是语法糖。
#define LOG_SYSERR   ::coro_net::Logger(__FILE__, __LINE__, false).stream()
#define LOG_SYSFATAL ::coro_net::Logger(__FILE__, __LINE__, true).stream()
