#pragma once
// Small thread-safe bounded queue used between FreeRTOS tasks.
#include <deque>
#include <mutex>

namespace platform {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t cap) : cap_(cap) {}
  // Drops the oldest element when full; returns false in that case.
  bool push(T v) {
    std::lock_guard<std::mutex> lk(m_);
    bool dropped = false;
    if (q_.size() >= cap_) {
      q_.pop_front();
      ++drops_;
      dropped = true;
    }
    q_.push_back(std::move(v));
    return !dropped;
  }
  bool pop(T& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (q_.empty()) return false;
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  size_t size() {
    std::lock_guard<std::mutex> lk(m_);
    return q_.size();
  }
  void clear() {
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
  }
  uint32_t drops() const { return drops_; }

 private:
  std::mutex m_;
  std::deque<T> q_;
  size_t cap_;
  uint32_t drops_ = 0;
};

}  // namespace platform
