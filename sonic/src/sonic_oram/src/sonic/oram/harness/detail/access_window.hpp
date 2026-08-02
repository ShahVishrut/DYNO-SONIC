#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sonic/threads/sync.hpp"

namespace sn::oram::harness::detail {

enum class window_close {
  none,
  flush,
  drop,
};

struct window_token {
  std::uint64_t window_index = 0;
  std::size_t slot_in_window = 0;
  bool active = false;
};

class access_window_tracker {
public:
  explicit access_window_tracker(std::size_t size) noexcept : size_(size) {}

  // each access reserves one slot in the current live window
  [[nodiscard]] window_token before_access() noexcept {
    window_token token{};
    if (size_ == 0) {
      return token;
    }

    std::size_t slot = 0;
    while (true) {
      if (closing_.load(std::memory_order_acquire)) {
        sn::threads::cpu_relax();
        continue;
      }
      slot = next_slot_.load(std::memory_order_acquire);
      if (slot >= size_) {
        sn::threads::cpu_relax();
        continue;
      }
      if (next_slot_.compare_exchange_weak(slot, slot + 1, std::memory_order_acq_rel)) {
        break;
      }
    }

    outstanding_.fetch_add(1, std::memory_order_acq_rel);
    token.window_index = window_index_.load(std::memory_order_acquire);
    token.slot_in_window = slot;
    token.active = true;
    return token;
  }

  [[nodiscard]] bool after_access(window_token token) noexcept {
    if (!token.active) {
      return false;
    }

    const bool window_full = (token.slot_in_window + 1 == size_);
    const bool was_last = outstanding_.fetch_sub(1, std::memory_order_acq_rel) == 1;
    if (!window_full) {
      return false;
    }

    bool expected = false;
    const bool became_closer = closing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    if (!became_closer) {
      while (closing_.load(std::memory_order_acquire)) {
        sn::threads::cpu_relax();
      }
      return false;
    }

    if (!was_last) {
      while (outstanding_.load(std::memory_order_acquire) != 0) {
        sn::threads::cpu_relax();
      }
    }
    return true;
  }

  [[nodiscard]] bool finalize() noexcept {
    if (size_ == 0) {
      return false;
    }

    const std::size_t used = next_slot_.load(std::memory_order_acquire);
    if (used == 0) {
      return false;
    }

    bool expected = false;
    const bool became_closer = closing_.compare_exchange_strong(expected, true, std::memory_order_acq_rel);
    if (!became_closer) {
      while (closing_.load(std::memory_order_acquire)) {
        sn::threads::cpu_relax();
      }
      return false;
    }

    while (outstanding_.load(std::memory_order_acquire) != 0) {
      sn::threads::cpu_relax();
    }

    return true;
  }

  void complete_close() noexcept {
    // the closer advances the window after flush/drop
    if (size_ == 0) {
      return;
    }

    window_index_.fetch_add(1, std::memory_order_acq_rel);
    next_slot_.store(0, std::memory_order_release);
    closing_.store(false, std::memory_order_release);
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] std::uint64_t current_window_index() const noexcept {
    return window_index_.load(std::memory_order_acquire);
  }

private:
  std::size_t size_ = 0;
  std::atomic<std::size_t> next_slot_{0};
  std::atomic<std::size_t> outstanding_{0};
  std::atomic<std::uint64_t> window_index_{0};
  std::atomic<bool> closing_{false};
};

} // namespace sn::oram::harness::detail
