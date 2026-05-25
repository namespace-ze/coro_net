// =============================================================================
// util/circular_buffer.h —— 循环缓冲区（拷自 mymuduo/utils/CircularBuffer.h）
// =============================================================================
//
// 【用途】
//   时间轮的桶容器：CircularBuffer<unordered_set<shared_ptr<IdleEntry>>>。
//   每秒 push_back 一个空桶；满了之后覆盖最旧的桶 → 旧桶里的元素引用计数减一
//   → 若没人续命则 IdleEntry 析构 → 触发关闭协程。
//
// 【迭代器】仅 forward 迭代器，遍历从 head 到 tail。本文件保留 mymuduo 原版
//          逻辑，只是改成了 namespace 内部使用。
//
// 【与 mymuduo 版的差异】
//   - 加 namespace coro_net::util
//   - #pragma once
//   - 删除 demo main 注释
// =============================================================================

#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace coro_net::util {

template <typename T>
class CircularBuffer {
public:
    class iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = T;
        using pointer = T*;
        using reference = T&;

        iterator(CircularBuffer* buffer, size_t pos)
            : buffer_(buffer), pos_(pos) {}

        reference operator*() const { return buffer_->buffer_[pos_]; }
        pointer operator->() const { return &buffer_->buffer_[pos_]; }

        iterator& operator++() {
            pos_ = buffer_->next(pos_);
            if (pos_ == buffer_->tail_ || buffer_->empty()) {
                buffer_ = nullptr;
                pos_ = 0;
            }
            return *this;
        }

        iterator operator++(int) {
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        friend bool operator==(const iterator& a, const iterator& b) {
            return a.buffer_ == b.buffer_ && a.pos_ == b.pos_;
        }
        friend bool operator!=(const iterator& a, const iterator& b) {
            return !(a == b);
        }

    private:
        CircularBuffer* buffer_;
        size_t pos_;
    };

    explicit CircularBuffer(size_t capacity)
        : capacity_(capacity), size_(0), head_(0), tail_(0) {
        buffer_.resize(capacity);
    }

    // push_back: 满时覆盖最旧元素（这是时间轮要的语义）
    void push_back(const T& item) {
        if (size_ == capacity_) {
            buffer_[tail_] = item;
            tail_ = next(tail_);
            head_ = tail_;
        } else {
            buffer_[tail_] = item;
            tail_ = next(tail_);
            ++size_;
        }
    }

    void push_back(T&& item) {
        if (size_ == capacity_) {
            buffer_[tail_] = std::move(item);
            tail_ = next(tail_);
            head_ = tail_;
        } else {
            buffer_[tail_] = std::move(item);
            tail_ = next(tail_);
            ++size_;
        }
    }

    T& back() {
        if (empty()) throw std::underflow_error("CircularBuffer is empty");
        return buffer_[prev(tail_)];
    }

    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }

    iterator begin() {
        if (empty()) return end();
        return iterator(this, head_);
    }
    iterator end() { return iterator(nullptr, 0); }

private:
    size_t next(size_t index) const { return (index + 1) % capacity_; }
    size_t prev(size_t index) const {
        return (index - 1 + capacity_) % capacity_;
    }

    std::vector<T> buffer_;
    size_t capacity_;
    size_t size_;
    size_t head_;
    size_t tail_;
};

}  // namespace coro_net::util
