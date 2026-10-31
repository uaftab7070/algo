#pragma once
// Fixed-capacity frame queue between capture and processing. For live sources
// overflow never blocks capture: the configured policy discards one frame and
// the discarded frame's identity is retained so the manifest can account for it.
// Finite file inputs use push_blocking() instead, so replay is lossless.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ahpv {

enum class DropPolicy { DropOldest, DropNewest };

inline const char* drop_policy_name(DropPolicy policy) {
    return policy == DropPolicy::DropOldest ? "drop_oldest" : "drop_newest";
}

inline DropPolicy parse_drop_policy(const std::string& name) {
    if (name == "drop_oldest") return DropPolicy::DropOldest;
    if (name == "drop_newest") return DropPolicy::DropNewest;
    throw std::invalid_argument("unknown drop policy '" + name + "'");
}

template <class T, class Summary>
class BoundedQueue {
public:
    // summarize() extracts what must be kept about a dropped item (not its pixels).
    using Summarize = Summary (*)(const T&);

    BoundedQueue(std::size_t capacity, DropPolicy policy, Summarize summarize)
        : capacity_(capacity), policy_(policy), summarize_(summarize) {
        if (capacity == 0) throw std::invalid_argument("queue capacity must be positive");
    }

    // Returns false only when the queue is closed.
    bool push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return false;
        if (items_.size() >= capacity_) {
            if (policy_ == DropPolicy::DropOldest) {
                dropped_.push_back(summarize_(items_.front()));
                items_.pop_front();
            } else {
                dropped_.push_back(summarize_(item));
                ++drops_;
                ready_.notify_one();
                return true;
            }
            ++drops_;
        }
        items_.push_back(std::move(item));
        if (items_.size() > high_water_) high_water_ = items_.size();
        ready_.notify_one();
        return true;
    }

    // Lossless backpressure for finite inputs (file replay): waits for space.
    // Returns false only when the queue is closed.
    bool push_blocking(T item) {
        std::unique_lock<std::mutex> lock(mutex_);
        space_.wait(lock, [&] { return closed_ || items_.size() < capacity_; });
        if (closed_) return false;
        items_.push_back(std::move(item));
        if (items_.size() > high_water_) high_water_ = items_.size();
        ready_.notify_one();
        return true;
    }

    // Waits up to timeout. Returns nullopt on timeout or when closed and empty.
    std::optional<T> pop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, timeout, [&] { return closed_ || !items_.empty() || !dropped_.empty(); });
        if (items_.empty()) return std::nullopt;
        T item = std::move(items_.front());
        items_.pop_front();
        space_.notify_one();
        return item;
    }

    std::vector<Summary> take_dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Summary> out(dropped_.begin(), dropped_.end());
        dropped_.clear();
        return out;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        ready_.notify_all();
        space_.notify_all();
    }

    bool closed_and_empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_ && items_.empty();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }
    std::size_t high_water() {
        std::lock_guard<std::mutex> lock(mutex_);
        return high_water_;
    }
    std::uint64_t drops() {
        std::lock_guard<std::mutex> lock(mutex_);
        return drops_;
    }
    std::size_t capacity() const { return capacity_; }
    DropPolicy policy() const { return policy_; }

private:
    const std::size_t capacity_;
    const DropPolicy policy_;
    const Summarize summarize_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<T> items_;
    std::deque<Summary> dropped_;
    std::size_t high_water_{0};
    std::uint64_t drops_{0};
    bool closed_{false};
};

}  // namespace ahpv
