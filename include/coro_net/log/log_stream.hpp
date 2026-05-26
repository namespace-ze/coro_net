// =============================================================================
// coro_net/log/log_stream.hpp —— LogStream + FixedBuffer
// =============================================================================
// 前端用的固定大小栈缓冲 + operator<< 链。零动态分配；满了截断不抛异常。
// 整数格式化走自实现的双字符表（参考 muduo Logging.h），比 snprintf 快约 8 倍。
// =============================================================================

#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace coro_net {

// -----------------------------------------------------------------------------
// FixedBuffer —— 编译期固定大小的栈/堆缓冲；不抛异常
// -----------------------------------------------------------------------------
template <int SIZE>
class FixedBuffer {
public:
    FixedBuffer() noexcept : cur_(data_) {}

    void append(const char* buf, size_t len) noexcept {
        size_t a = static_cast<size_t>(avail());
        if (len > a) len = a;  // 满了截断
        std::memcpy(cur_, buf, len);
        cur_ += len;
    }

    const char* data() const noexcept { return data_; }
    int length() const noexcept { return static_cast<int>(cur_ - data_); }

    char* current() noexcept { return cur_; }
    int avail() const noexcept { return static_cast<int>(end() - cur_); }
    void add(size_t n) noexcept { cur_ += n; }

    void reset() noexcept { cur_ = data_; }
    void bzero() noexcept { std::memset(data_, 0, sizeof data_); }

    static constexpr int capacity() noexcept { return SIZE; }

private:
    const char* end() const noexcept { return data_ + sizeof data_; }

    char data_[SIZE];
    char* cur_;
};

// 别名：前端用 4000B（一行日志足够），后端用 4MB（多行批量刷盘）
using SmallBuffer = FixedBuffer<4000>;
using LargeBuffer = FixedBuffer<4 * 1024 * 1024>;

// -----------------------------------------------------------------------------
// LogStream —— 前端 << 链。所有 operator<< 都返回自身引用便于链式书写。
// -----------------------------------------------------------------------------
class LogStream {
public:
    using self = LogStream;

    self& operator<<(bool v) noexcept {
        buffer_.append(v ? "1" : "0", 1);
        return *this;
    }

    self& operator<<(short v) noexcept;
    self& operator<<(unsigned short v) noexcept;
    self& operator<<(int v) noexcept;
    self& operator<<(unsigned int v) noexcept;
    self& operator<<(long v) noexcept;
    self& operator<<(unsigned long v) noexcept;
    self& operator<<(long long v) noexcept;
    self& operator<<(unsigned long long v) noexcept;

    self& operator<<(float v) noexcept { return *this << static_cast<double>(v); }
    self& operator<<(double v) noexcept;

    self& operator<<(char v) noexcept {
        buffer_.append(&v, 1);
        return *this;
    }

    self& operator<<(const char* str) noexcept {
        if (str) {
            buffer_.append(str, std::strlen(str));
        } else {
            buffer_.append("(null)", 6);
        }
        return *this;
    }

    self& operator<<(const unsigned char* str) noexcept {
        return *this << reinterpret_cast<const char*>(str);
    }

    self& operator<<(const std::string& s) noexcept {
        buffer_.append(s.data(), s.size());
        return *this;
    }

    self& operator<<(std::string_view sv) noexcept {
        buffer_.append(sv.data(), sv.size());
        return *this;
    }

    self& operator<<(const void* p) noexcept;

    void append(const char* data, size_t len) noexcept { buffer_.append(data, len); }
    const SmallBuffer& buffer() const noexcept { return buffer_; }
    void reset_buffer() noexcept { buffer_.reset(); }

    static constexpr int kMaxNumericSize = 48;

private:
    SmallBuffer buffer_;
};

}  // namespace coro_net
