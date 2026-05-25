// =============================================================================
// coro_net/buffer.hpp —— 用户态读写缓冲区
// =============================================================================
//
// 【与 mymuduo Buffer 的差异】
//   - 删除了 readFd / writeFd（io_uring awaiter 接管 IO，不需要这两个 syscall 包装）
//   - namespace 改为 coro_net
//   - 接口保留：peek / readableBytes / writableBytes / retrieve / retrieveAsString /
//             append / ensureWritableBytes / beginWrite / makeSpace
//
// 【结构】
//   | kCheapPrepend (8B) | prependable | readable | writable |
//   0                  readerIndex   writerIndex             size
//
//   - readable bytes：已收到、待应用层消费的数据
//   - writable bytes：可向其追加新数据的空闲区
//   - kCheapPrepend：固定 8B 保留区，便于在协议头前插 4B 长度（RPC 拆帧用）
//
// 【典型用法】（在协程内）
//   ssize_t n = co_await RecvIntoBuffer{conn.fd(), buf};
//   while (buf.readableBytes() >= 4) {
//       uint32_t hdr_size;
//       std::memcpy(&hdr_size, buf.peek(), 4);
//       if (buf.readableBytes() < 4 + hdr_size) break;   // 半包，等下一次
//       std::string hdr_str(buf.peek() + 4, hdr_size);
//       buf.retrieve(4 + hdr_size);
//       // ... 处理 hdr ...
//   }
//
// 【为什么不直接复用 mymuduo Buffer.h】
//   - mymuduo Buffer.h 含 readFd/writeFd，引入 sys/uio.h 依赖；
//   - 命名 namespace 与对外暴露形式不一致；
//   - 拷一份保证 coro_net 独立可移植（详见 plan §复用 mymuduo 组件）。
// =============================================================================

#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace coro_net {

class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;     // 4B 长度前缀 + 对齐
    static constexpr size_t kInitialSize = 1024;

    explicit Buffer(size_t initialSize = kInitialSize)
        : buffer_(kCheapPrepend + initialSize),
          readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes() const noexcept {
        return writerIndex_ - readerIndex_;
    }
    size_t writableBytes() const noexcept {
        return buffer_.size() - writerIndex_;
    }
    size_t prependableBytes() const noexcept { return readerIndex_; }

    // 返回 readable 区域的起始地址
    const char* peek() const noexcept { return begin() + readerIndex_; }
    char* peek() noexcept { return begin() + readerIndex_; }

    // 从 readable 区域取走 len 字节（推进 readerIndex_）
    void retrieve(size_t len) noexcept {
        if (len < readableBytes()) {
            readerIndex_ += len;
        } else {
            retrieveAll();
        }
    }

    void retrieveAll() noexcept {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string retrieveAllAsString() {
        return retrieveAsString(readableBytes());
    }
    std::string retrieveAsString(size_t len) {
        std::string s(peek(), len);
        retrieve(len);
        return s;
    }

    void ensureWritableBytes(size_t len) {
        if (writableBytes() < len) makeSpace(len);
    }

    // 把 [data, data+len) 追加到 writable 区域
    void append(const char* data, size_t len) {
        ensureWritableBytes(len);
        std::memcpy(beginWrite(), data, len);
        writerIndex_ += len;
    }
    void append(const std::string& s) {
        append(s.data(), s.size());
    }
    void append(const void* p, size_t len) {
        append(static_cast<const char*>(p), len);
    }

    char* beginWrite() noexcept { return begin() + writerIndex_; }
    const char* beginWrite() const noexcept { return begin() + writerIndex_; }

private:
    char* begin() noexcept { return buffer_.data(); }
    const char* begin() const noexcept { return buffer_.data(); }

    // 调整 writable 空间
    void makeSpace(size_t len) {
        if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
            // 总空间不够，扩容
            buffer_.resize(writerIndex_ + len);
        } else {
            // 把 readable 数据搬到 kCheapPrepend 起点，腾出 writable 空间
            size_t readable = readableBytes();
            std::memmove(begin() + kCheapPrepend,
                         begin() + readerIndex_,
                         readable);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    std::vector<char> buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};

}  // namespace coro_net
