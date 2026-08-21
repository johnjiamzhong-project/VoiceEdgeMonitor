#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace voiceedge {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      throw std::invalid_argument("bounded queue capacity must be greater than zero");
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  bool try_push(T item) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_ || queue_.size() >= capacity_) {
        return false;
      }
      queue_.push_back(std::move(item));
    }
    not_empty_.notify_one();
    return true;
  }

  // Returns true when an older item had to be dropped to make room.
  bool push_drop_oldest(T item) {
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        return false;
      }
      if (queue_.size() >= capacity_) {
        queue_.pop_front();
        ++dropped_items_;
        dropped = true;
      }
      queue_.push_back(std::move(item));
    }
    not_empty_.notify_one();
    return dropped;
  }

  template <typename Rep, typename Period>
  bool pop_for(T& item, const std::chrono::duration<Rep, Period>& timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait_for(lock, timeout, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    item = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  bool pop(T& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    item = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t capacity() const { return capacity_; }

  [[nodiscard]] std::uint64_t dropped_items() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_items_;
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::deque<T> queue_;
  std::uint64_t dropped_items_{0};
  bool closed_{false};
};

}  // namespace voiceedge
