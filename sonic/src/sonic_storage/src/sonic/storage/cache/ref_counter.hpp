#pragma once

#include <atomic>

#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"

namespace sn::storage::cache {

struct ref_counter {
  static constexpr std::uint32_t k_exclusive = 0x80000000u;
  static constexpr std::uint32_t k_shared_mask = ~k_exclusive;

  std::atomic<std::uint32_t> word{0};

  void set_first_shared() noexcept { word.store(1U, std::memory_order_relaxed); }
  void set_first_exclusive() noexcept { word.store(k_exclusive | 1U, std::memory_order_relaxed); }

  void add_shared() noexcept {
    for (;;) {
      std::uint32_t cur = word.load(std::memory_order_acquire);
      if ((cur & k_exclusive) != 0U) {
        sn::threads::thread_yield();
        continue;
      }
      if (word.compare_exchange_weak(cur, cur + 1U, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
      }
    }
  }

  void acquire_exclusive() noexcept {
    for (;;) {
      std::uint32_t cur = word.load(std::memory_order_acquire);
      if ((cur & k_exclusive) != 0U) {
        sn::threads::thread_yield();
        continue;
      }
      if (word.compare_exchange_weak(cur, cur | k_exclusive, std::memory_order_acq_rel, std::memory_order_acquire)) {
        while ((word.load(std::memory_order_acquire) & k_shared_mask) != 0U) {
          sn::threads::thread_yield();
        }
        word.store(k_exclusive | 1U, std::memory_order_release);
        return;
      }
    }
  }

  void release_shared() noexcept {
    const auto prev = word.fetch_sub(1U, std::memory_order_release);
    sn::util::log::ensure((prev & k_shared_mask) > 0U, "cache invariant");
  }

  void release_exclusive() noexcept {
    const auto prev = word.exchange(0U, std::memory_order_release);
    sn::util::log::ensure((prev & k_exclusive) != 0U, "cache invariant");
  }

  [[nodiscard]] bool idle() const noexcept { return word.load(std::memory_order_acquire) == 0U; }
};

}
