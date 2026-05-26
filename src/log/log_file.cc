// =============================================================================
// log_file.cc —— AppendFile + LogFile 实现
// =============================================================================
#include "coro_net/log/log_file.hpp"

#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace coro_net {

// -----------------------------------------------------------------------------
// AppendFile
// -----------------------------------------------------------------------------
AppendFile::AppendFile(const std::string& filename)
    : fp_(::fopen(filename.c_str(), "ae")) {  // 'a' = append, 'e' = O_CLOEXEC
    if (fp_) {
        ::setbuffer(fp_, buffer_, sizeof buffer_);
    }
}

AppendFile::~AppendFile() {
    if (fp_) ::fclose(fp_);
}

void AppendFile::append(const char* logline, size_t len) noexcept {
    if (!fp_) return;
    size_t written = 0;
    while (written != len) {
        size_t remain = len - written;
        size_t n = write(logline + written, remain);
        if (n != remain) {
            int err = ::ferror(fp_);
            if (err) {
                char errbuf[128];
                std::fprintf(stderr, "AppendFile::append() failed %s\n",
                             ::strerror_r(err, errbuf, sizeof errbuf));
                break;
            }
        }
        written += n;
    }
    written_bytes_ += static_cast<off_t>(written);
}

void AppendFile::flush() noexcept {
    if (fp_) ::fflush(fp_);
}

size_t AppendFile::write(const char* logline, size_t len) noexcept {
    return ::fwrite_unlocked(logline, 1, len, fp_);
}

// -----------------------------------------------------------------------------
// LogFile
// -----------------------------------------------------------------------------
LogFile::LogFile(std::string basename, off_t roll_size,
                 int flush_interval_seconds, int check_every_n)
    : basename_(std::move(basename)),
      roll_size_(roll_size),
      flush_interval_(flush_interval_seconds),
      check_every_n_(check_every_n) {
    roll_file();
}

void LogFile::append(const char* logline, int len) noexcept {
    if (!file_) return;
    file_->append(logline, static_cast<size_t>(len));

    if (file_->written_bytes() > roll_size_) {
        roll_file();
        return;
    }

    ++count_;
    if (count_ >= check_every_n_) {
        count_ = 0;
        time_t now = ::time(nullptr);
        time_t this_period = now / kRollPerSeconds * kRollPerSeconds;
        if (this_period != start_of_period_) {
            roll_file();
        } else if (now - last_flush_ > flush_interval_) {
            last_flush_ = now;
            file_->flush();
        }
    }
}

void LogFile::flush() noexcept {
    if (file_) file_->flush();
}

std::string LogFile::make_filename(time_t now) {
    std::string filename;
    filename.reserve(basename_.size() + 64);
    filename = basename_;

    char timebuf[32];
    struct tm tm_buf;
    ::gmtime_r(&now, &tm_buf);
    std::strftime(timebuf, sizeof timebuf, ".%Y%m%d-%H%M%S", &tm_buf);
    filename += timebuf;

    // 同一秒内多次 roll：追加序号后缀，确保文件名唯一
    if (now == last_roll_) {
        ++same_second_seq_;
        char seqbuf[16];
        std::snprintf(seqbuf, sizeof seqbuf, "-%d", same_second_seq_);
        filename += seqbuf;
    } else {
        same_second_seq_ = 0;
    }
    filename += ".";

    char host[64] = "unknownhost";
    if (::gethostname(host, sizeof host) == 0) {
        host[sizeof host - 1] = '\0';
    }
    filename += host;

    char pidbuf[32];
    std::snprintf(pidbuf, sizeof pidbuf, ".%d", ::getpid());
    filename += pidbuf;

    filename += ".log";
    return filename;
}

bool LogFile::roll_file() {
    time_t now = ::time(nullptr);
    // make_filename 内部根据 (now == last_roll_) 决定是否追加 -seq 后缀
    std::string name = make_filename(now);
    time_t start = now / kRollPerSeconds * kRollPerSeconds;
    if (now >= last_roll_) {
        last_roll_ = now;
        last_flush_ = now;
        start_of_period_ = start;
        file_ = std::make_unique<AppendFile>(name);
        return true;
    }
    return false;
}

}  // namespace coro_net
