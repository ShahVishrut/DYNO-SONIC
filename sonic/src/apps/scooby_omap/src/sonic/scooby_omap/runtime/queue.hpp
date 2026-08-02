#pragma once

#include <cstddef>
#include <deque>
#include <utility>

#include "sonic/threads/sync.hpp"

namespace sn::scooby::omap {

namespace sync = sn::threads;

template <typename T> class blocking_queue {
public:
  blocking_queue() = default;

  void configure(const char* role, const char* name) {
    role_ = role;
    name_ = name;
  }

  void push(T value) {
    {
      sync::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      queue_.push_back(std::move(value));
      if (queue_.size() > max_depth_) {
        max_depth_ = queue_.size();
      }
    }
    cv_.notify_one();
  }

  bool pop(T& out) {
    sync::unique_lock<sync::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
      return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    {
      sync::lock_guard lock(mutex_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  std::size_t max_depth() const {
    sync::lock_guard lock(mutex_);
    return max_depth_;
  }

  const char* role() const { return role_; }
  const char* name() const { return name_; }

private:
  std::deque<T> queue_{};
  mutable sync::mutex mutex_{};
  sync::condition_variable cv_{};
  bool closed_{false};
  std::size_t max_depth_{0};
  const char* role_{"role"};
  const char* name_{"queue"};
};

}
