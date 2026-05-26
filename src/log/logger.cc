// =============================================================================
// logger.cc —— Logger 实现：前缀格式化 + TLS 时间/tid 缓存 + 析构刷入后端
// =============================================================================
#include "coro_net/log/logger.hpp"
#include "coro_net/log/async_logger.hpp"

#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace coro_net {

// -----------------------------------------------------------------------------
// 运行期级别（默认 INFO）
// -----------------------------------------------------------------------------
std::atomic<LogLevel> Logger::global_level_{LogLevel::INFO};

// -----------------------------------------------------------------------------
// TLS 缓存：tid 字符串 + 上一秒的 YYYYMMDD HH:MM:SS
// -----------------------------------------------------------------------------
namespace {
thread_local int  t_cached_tid = 0;
thread_local char t_tid_str[32] = {0};
thread_local int  t_tid_len = 0;

thread_local time_t t_last_second = 0;
thread_local char   t_time_str[32] = {0};  // "YYYYMMDD HH:MM:SS"

void cache_tid() noexcept {
    if (t_cached_tid == 0) {
        t_cached_tid = static_cast<int>(::syscall(SYS_gettid));
        t_tid_len = std::snprintf(t_tid_str, sizeof t_tid_str, "%5d", t_cached_tid);
    }
}

// 把 "YYYYMMDD HH:MM:SS.uuuuuu " 写到 buf，返回写入字节数
size_t format_timestamp(char* buf, size_t cap) noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t sec = ts.tv_sec;
    long usec = ts.tv_nsec / 1000;

    if (sec != t_last_second) {
        t_last_second = sec;
        struct tm tm_buf;
        localtime_r(&sec, &tm_buf);
        std::snprintf(t_time_str, sizeof t_time_str,
                      "%04d%02d%02d %02d:%02d:%02d",
                      tm_buf.tm_year + 1900,
                      tm_buf.tm_mon + 1,
                      tm_buf.tm_mday,
                      tm_buf.tm_hour,
                      tm_buf.tm_min,
                      tm_buf.tm_sec);
    }
    int n = std::snprintf(buf, cap, "%s.%06ld ", t_time_str, usec);
    return n > 0 ? static_cast<size_t>(n) : 0;
}
}  // namespace

// -----------------------------------------------------------------------------
// 构造：写入前缀（时间 tid level）
// -----------------------------------------------------------------------------
Logger::Logger(SourceFile file, int line, LogLevel level) noexcept
    : level_(level), line_(line), file_(file) {
    format_prefix(level);
}

Logger::Logger(SourceFile file, int line, LogLevel level, const char* func) noexcept
    : level_(level), line_(line), file_(file) {
    format_prefix(level);
    if (func) {
        stream_ << func << ' ';
    }
}

Logger::Logger(SourceFile file, int line, bool toAbort) noexcept
    : level_(toAbort ? LogLevel::FATAL : LogLevel::ERROR),
      line_(line), file_(file), to_abort_(toAbort) {
    format_prefix(level_);
    int e = errno;
    char errbuf[128];
    const char* es = ::strerror_r(e, errbuf, sizeof errbuf);
    stream_ << "(errno=" << e << ": " << (es ? es : "?") << ") ";
}

void Logger::format_prefix(LogLevel level) noexcept {
    char ts[40];
    size_t n = format_timestamp(ts, sizeof ts);
    stream_.append(ts, n);

    cache_tid();
    stream_.append(t_tid_str, static_cast<size_t>(t_tid_len));
    stream_.append(" ", 1);

    stream_.append(log_level_name(level), 6);
}

// -----------------------------------------------------------------------------
// 析构：补 " - file:line\n"，提交到后端；FATAL/SYSFATAL 则 abort
// -----------------------------------------------------------------------------
Logger::~Logger() noexcept {
    stream_ << " - " << file_.data_ << ':' << line_ << '\n';
    const auto& buf = stream_.buffer();
    AsyncLogger::instance().append(buf.data(), buf.length());

    if (level_ == LogLevel::FATAL || to_abort_) {
        // 把当前批次推到后端、再 stop 等 backend join，确保最后一行落盘
        AsyncLogger::instance().flush_and_stop();
        std::abort();
    }
}

}  // namespace coro_net
