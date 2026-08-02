#pragma once

#include <cstdint>
#include <iterator>
#include <list>
#include <vector>

#include "sonic/threads/sync.hpp"

namespace sn::storage::cache {

class lru_policy {
public:
  explicit lru_policy(std::uint32_t frame_count = 0) : iters_(frame_count, order_.end()) {
    for (std::uint32_t i = 0; i < frame_count; ++i) {
      add(i);
    }
  }

  void add(std::uint32_t frame_id) {
    sn::threads::lock_guard lock(mtx_);
    if (frame_id >= iters_.size()) {
      iters_.resize(frame_id + 1, order_.end());
    }
    if (iters_[frame_id] != order_.end()) {
      order_.erase(iters_[frame_id]);
    }
    order_.push_back(frame_id);
    iters_[frame_id] = std::prev(order_.end());
  }

  void touch(std::uint32_t frame_id) {
    sn::threads::lock_guard lock(mtx_);
    if (frame_id >= iters_.size() || iters_[frame_id] == order_.end()) {
      return;
    }
    order_.splice(order_.end(), order_, iters_[frame_id]);
    iters_[frame_id] = std::prev(order_.end());
  }

  void remove(std::uint32_t frame_id) {
    sn::threads::lock_guard lock(mtx_);
    if (frame_id >= iters_.size() || iters_[frame_id] == order_.end()) {
      return;
    }
    order_.erase(iters_[frame_id]);
    iters_[frame_id] = order_.end();
  }

  [[nodiscard]] std::uint32_t choose_victim() {
    sn::threads::lock_guard lock(mtx_);
    if (order_.empty()) {
      return k_invalid;
    }
    return order_.front();
  }

  static constexpr std::uint32_t k_invalid = static_cast<std::uint32_t>(-1);

private:
  std::list<std::uint32_t> order_;
  std::vector<std::list<std::uint32_t>::iterator> iters_;
  sn::threads::mutex mtx_;
};

}
