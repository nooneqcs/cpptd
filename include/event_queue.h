#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace ctp {

// 回调线程 push，主线程 pop_wait + tick
template <class T>
class EventQueue {
public:
    void push(const T& e) {
        { std::lock_guard<std::mutex> lk(m_); q_.push_back(e); }
        cv_.notify_one();
    }

    bool pop_wait(T& e, int ms) {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !q_.empty(); }))
            return false;
        e = q_.front();
        q_.pop_front();
        return true;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return q_.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
    }

private:
    std::deque<T> q_;
    mutable std::mutex m_;
    std::condition_variable cv_;
};

} // namespace ctp

