// =============================================================================
// coro_net/log/log_file.hpp —— 滚动落盘文件
// =============================================================================
// 仅由后端线程使用，无锁。按大小（默认 64MB）或按天滚动。
// 文件名：basename.YYYYMMDD-HHMMSS.host.pid.log
// =============================================================================

#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <ctime>
#include <sys/types.h>

namespace coro_net {

// 简单封装 FILE*：开起来设 64KB 用户态缓冲，写入用 fwrite_unlocked
class AppendFile {
public:
    explicit AppendFile(const std::string& filename);
    ~AppendFile();

    AppendFile(const AppendFile&) = delete;
    AppendFile& operator=(const AppendFile&) = delete;

    void append(const char* logline, size_t len) noexcept;
    void flush() noexcept;
    off_t written_bytes() const noexcept { return written_bytes_; }

private:
    size_t write(const char* logline, size_t len) noexcept;

    FILE* fp_;
    char  buffer_[64 * 1024];
    off_t written_bytes_ = 0;
};

class LogFile {
public:
    LogFile(std::string basename,
            off_t roll_size = 64 * 1024 * 1024,
            int flush_interval_seconds = 3,
            int check_every_n = 1024);
    ~LogFile() = default;

    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    void append(const char* logline, int len) noexcept;
    void flush() noexcept;
    bool roll_file();

private:
    std::string make_filename(time_t now);

    const std::string basename_;
    const off_t roll_size_;
    const int   flush_interval_;
    const int   check_every_n_;

    int    count_ = 0;
    int    same_second_seq_ = 0;   // 同一秒内多次 roll 时区分文件名
    time_t start_of_period_ = 0;
    time_t last_roll_ = 0;
    time_t last_flush_ = 0;

    std::unique_ptr<AppendFile> file_;

    static constexpr int kRollPerSeconds = 60 * 60 * 24;  // 每天
};

}  // namespace coro_net
