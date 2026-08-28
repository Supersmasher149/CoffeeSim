#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace espressolab {

// A bounded, blocking multi-producer/single-consumer queue (issue #38). Any
// number of producer threads may call push(); exactly one consumer thread
// drains it with pop(). This type owns no threads itself and never resizes
// -- callers choose (and may override) the capacity when they construct one.
// See apps/espressolab_cli/sweep_batch_runner.* for the producer/consumer
// wiring this exists for.
template <typename T>
class BoundedRingBuffer {
public:
    explicit BoundedRingBuffer(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("BoundedRingBuffer capacity must be at least 1");
        }
    }

    // Blocks while the buffer is full. Safe to call from multiple producer
    // threads concurrently. A push racing a close() (a caller bug -- every
    // producer must stop before the last one closes the buffer) drops the
    // value rather than reopening the buffer.
    void push(T value) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] { return items_.size() < capacity_ || closed_; });
        if (closed_) return;
        items_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
    }

    // Blocks while the buffer is empty and still open. Returns false once
    // the buffer has been closed and fully drained -- the consumer's stop
    // signal. Only one thread may call pop() (single consumer).
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] { return !items_.empty() || closed_; });
        if (items_.empty()) return false;  // closed and drained
        out = std::move(items_.front());
        items_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    // Idempotent. Called once, after the last producer has finished
    // pushing, so the consumer can observe end-of-stream once it drains
    // whatever is still queued.
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t capacity() const { return capacity_; }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
    std::size_t capacity_;
    bool closed_ = false;
};

}  // namespace espressolab
