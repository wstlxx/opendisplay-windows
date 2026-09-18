// Bounded video queue between the network read thread and the decode thread.
// Capacity 4; when full the OLDEST frame is dropped (we can never use it any
// more — the display is already 4+ frames ahead of us).

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "../net/session.h"

namespace od::app {

class VideoQueue {
public:
    static constexpr size_t kCapacity = 4;

    explicit VideoQueue(size_t capacity = kCapacity) : capacity_(capacity) {}

    // Push a frame; drops the oldest if at capacity. Returns true if a
    // frame was dropped.
    bool Push(net::VideoSample&& sample) {
        std::lock_guard lock(mutex_);
        bool dropped = false;
        if (queue_.size() >= capacity_) {
            queue_.pop_front();
            dropped = true;
        }
        queue_.push_back(std::move(sample));
        if (dropped) ++dropped_;
        cond_.notify_one();
        return dropped;
    }

    // Blocks up to timeoutMs; returns the next frame if available.
    std::optional<net::VideoSample> PopWait(int timeoutMs) {
        std::unique_lock lock(mutex_);
        if (!cond_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return !queue_.empty() || shutting_down_; })) {
            return std::nullopt;
        }
        if (queue_.empty()) return std::nullopt; // shutting down
        net::VideoSample s = std::move(queue_.front());
        queue_.pop_front();
        return s;
    }

    void Shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutting_down_ = true;
        }
        cond_.notify_all();
    }

    size_t Size() {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    uint64_t Dropped() {
        std::lock_guard lock(mutex_);
        return dropped_;
    }

private:
    size_t capacity_;
    std::deque<net::VideoSample> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool shutting_down_ = false;
    uint64_t dropped_ = 0;
};

} // namespace od::app
