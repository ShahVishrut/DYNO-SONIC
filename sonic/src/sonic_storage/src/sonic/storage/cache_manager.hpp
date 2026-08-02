#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <unordered_map>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/threads/sync.hpp"

#include "sonic/storage/cache/eviction_policy.hpp"
#include "sonic/storage/cache/ref_counter.hpp"
#include "sonic/storage/cache/stats.hpp"
#include "sonic/storage/io/backend.hpp"
#include "sonic/storage/pin_mode.hpp"

namespace sn::storage {

template <typename Block> class cache_manager {
public:
  using block_t = Block;

  struct config {

    std::uint32_t blocks_per_page = 0;

    std::uint32_t frame_count = 0;
    bool enable_prefetch = false;

    std::uint32_t batch_write_limit = 32;
  };

  struct cache_frame {
    enum class state { free, loading, resident, evicting };

    block_t* base = nullptr;
    std::uint64_t page_id = 0;
    cache::ref_counter refs{};
    std::atomic<bool> dirty{false};
    std::atomic<state> st{state::free};

    cache_frame() = default;
    cache_frame(const cache_frame&) = delete;
    cache_frame& operator=(const cache_frame&) = delete;
    cache_frame(cache_frame&& other) noexcept { move_from(other); }
    cache_frame& operator=(cache_frame&& other) noexcept {
      if (this != &other) {
        move_from(other);
      }
      return *this;
    }

    static void reset_state(cache_frame& f) noexcept {
      f.page_id = 0;
      f.dirty.store(false, std::memory_order_relaxed);
      f.refs.word.store(0U, std::memory_order_relaxed);
      f.st.store(state::free, std::memory_order_relaxed);
    }

  private:
    void move_from(cache_frame& other) noexcept {
      base = other.base;
      page_id = other.page_id;
      refs.word.store(other.refs.word.load(std::memory_order_relaxed), std::memory_order_relaxed);
      dirty.store(other.dirty.load(std::memory_order_relaxed), std::memory_order_relaxed);
      st.store(other.st.load(std::memory_order_relaxed), std::memory_order_relaxed);

      other.base = nullptr;
      reset_state(other);
    }
  };

  class guard_t {
  public:
    guard_t() = default;
    guard_t(cache_frame* frame, block_t* ptr, std::uint32_t len, bool exclusive) noexcept :
        frame_(frame), ptr_(ptr), len_(len), exclusive_(exclusive) {}

    guard_t(const guard_t&) = delete;
    guard_t& operator=(const guard_t&) = delete;

    guard_t(guard_t&& other) noexcept :
        frame_(other.frame_), ptr_(other.ptr_), len_(other.len_), exclusive_(other.exclusive_) {
      other.frame_ = nullptr;
      other.ptr_ = nullptr;
      other.len_ = 0;
      other.exclusive_ = false;
    }

    guard_t& operator=(guard_t&& other) noexcept {
      if (this != &other) {
        cleanup();
        frame_ = other.frame_;
        ptr_ = other.ptr_;
        len_ = other.len_;
        exclusive_ = other.exclusive_;
        other.frame_ = nullptr;
        other.ptr_ = nullptr;
        other.len_ = 0;
        other.exclusive_ = false;
      }
      return *this;
    }

    ~guard_t() noexcept { cleanup(); }

    [[nodiscard]] sn::util::span<block_t> span() const noexcept {
      return sn::util::span<block_t>(ptr_, static_cast<std::size_t>(len_));
    }
    [[nodiscard]] block_t* data() const noexcept { return ptr_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return len_; }

    void mark_dirty() noexcept {
      if (frame_ != nullptr) {
        frame_->dirty.store(true, std::memory_order_relaxed);
      }
    }

  private:
    void cleanup() noexcept {
      if (frame_ != nullptr) {
        if (exclusive_) {
          frame_->refs.release_exclusive();
        } else {
          frame_->refs.release_shared();
        }
        frame_ = nullptr;
        ptr_ = nullptr;
        len_ = 0;
        exclusive_ = false;
      }
    }

    cache_frame* frame_ = nullptr;
    block_t* ptr_ = nullptr;
    std::uint32_t len_ = 0;
    bool exclusive_ = false;
  };

  cache_manager() = default;

  cache_manager(config cfg, std::unique_ptr<io::backend> backend) :
      cfg_(cfg), backend_(std::move(backend)), policy_(cfg.frame_count) {
    namespace log = sn::util::log;
    log::ensure(cfg_.blocks_per_page > 0, "cache_manager: blocks_per_page must be positive");
    log::ensure(cfg_.frame_count > 0, "cache_manager: frame_count must be positive");
    const std::size_t page_blocks = static_cast<std::size_t>(cfg_.blocks_per_page);
    const std::size_t page_bytes = page_blocks * sizeof(block_t);
    const std::size_t total_blocks = static_cast<std::size_t>(cfg_.frame_count) * page_blocks;
    const std::size_t alignment = std::max<std::size_t>(4096, alignof(block_t));
    backing_ = allocate_aligned(total_blocks, alignment);
    frames_.reserve(cfg_.frame_count);
    for (std::uint32_t i = 0; i < cfg_.frame_count; ++i) {
      frames_.emplace_back();
      cache_frame& f = frames_.back();
      f.base = backing_.get() + static_cast<std::size_t>(i) * page_blocks;
      f.st.store(cache_frame::state::free, std::memory_order_relaxed);
      f.dirty.store(false, std::memory_order_relaxed);
      f.refs.word.store(0U, std::memory_order_relaxed);
      policy_.add(i);
    }
    page_blocks_ = page_blocks;
    page_bytes_ = page_bytes;

    free_frame_indices_.reserve(cfg_.frame_count);
    for (std::uint32_t i = cfg_.frame_count; i > 0; --i) {
      free_frame_indices_.push_back(i - 1);
    }
  }

  [[nodiscard]] guard_t pin_shared(std::uint64_t page_id) { return pin(page_id, pin_mode::shared); }
  [[nodiscard]] guard_t pin_exclusive(std::uint64_t page_id) { return pin(page_id, pin_mode::exclusive); }

  [[nodiscard]] guard_t pin_exclusive_no_load(std::uint64_t page_id) { return pin_no_load(page_id); }

  void flush_dirty() {
    bool any_dirty = false;
    std::vector<std::uint32_t> batch_frames;
    batch_frames.reserve(cfg_.batch_write_limit ? cfg_.batch_write_limit : 16);
    std::vector<io::backend::page_view> batch_io;
    batch_io.reserve(batch_frames.capacity());

    while (true) {
      {
        sn::threads::lock_guard map_lock(map_mtx_);
        for (std::uint32_t i = 0; i < frames_.size(); ++i) {
          auto& frame = frames_[i];
          if (frame.st.load(std::memory_order_acquire) != cache_frame::state::resident) {
            continue;
          }

          if (!frame.dirty.load(std::memory_order_relaxed)) {
            continue;
          }

          if (!frame.refs.idle()) {
            continue;
          }

          frame.refs.set_first_shared();
          batch_frames.push_back(i);
          batch_io.push_back(io::backend::page_view{frame.page_id, frame.base, page_bytes_});

          if (batch_frames.size() >= cfg_.batch_write_limit) {
            break;
          }
        }
      }

      if (batch_frames.empty()) {
        break;
      }

      try {
        backend_->write_pages(sn::util::span<const io::backend::page_view>(batch_io.data(), batch_io.size()));
      } catch (...) {
        for (std::uint32_t idx : batch_frames) {
          frames_[idx].refs.release_shared();
        }
        throw;
      }
      stats_.write_ios.fetch_add(static_cast<std::uint64_t>(batch_io.size()), std::memory_order_relaxed);
      stats_.write_bytes.fetch_add(page_bytes_ * batch_io.size(), std::memory_order_relaxed);
      stats_.writebacks.fetch_add(static_cast<std::uint64_t>(batch_io.size()), std::memory_order_relaxed);
      any_dirty = true;

      for (std::uint32_t idx : batch_frames) {
        auto& f = frames_[idx];
        f.dirty.store(false, std::memory_order_release);
        f.refs.release_shared();
      }

      batch_frames.clear();
      batch_io.clear();
    }

    if (any_dirty) {
      backend_->flush();
      stats_.flushes.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  io::backend* backend() noexcept { return backend_.get(); }
  const io::backend* backend() const noexcept { return backend_.get(); }

  [[nodiscard]] std::uint64_t eviction_count() const noexcept {
    return eviction_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] cache::stats_snapshot stats_snapshot() const noexcept { return stats_.snapshot(); }
  void stats_reset() noexcept { stats_.reset(); }

  void invalidate_all(bool flush_dirty_pages = true) {
    if (flush_dirty_pages) {
      flush_dirty();
    }
    sn::threads::unique_lock<sn::threads::mutex> map_lock(map_mtx_);
    for (std::uint32_t i = 0; i < frames_.size(); ++i) {
      cache_frame& f = frames_[i];

      while (f.st.load(std::memory_order_acquire) == cache_frame::state::evicting) {
        eviction_cv_.wait(map_lock);
      }
      sn::util::log::ensure(f.refs.idle(), "cache invariant");
      f.page_id = 0;
      f.dirty.store(false, std::memory_order_relaxed);
      f.refs.word.store(0U, std::memory_order_relaxed);
      f.st.store(cache_frame::state::free, std::memory_order_release);
    }
    page_table_.clear();
    policy_.~lru_policy();
    new (&policy_) cache::lru_policy(cfg_.frame_count);

    free_frame_indices_.clear();
    for (std::uint32_t i = frames_.size(); i > 0; --i) {
      free_frame_indices_.push_back(i - 1);
    }
  }

  void prefetch(std::uint64_t page_id) {
    if (!cfg_.enable_prefetch) {
      return;
    }
    stats_.prefetches.fetch_add(1U, std::memory_order_relaxed);

    thread_local std::uint64_t last_hint_page = std::numeric_limits<std::uint64_t>::max();
    if (last_hint_page == page_id) {
      return;
    }
    last_hint_page = page_id;
    if (backend_ != nullptr) {
      backend_->hint_prefetch(page_id, page_bytes_);
    }
  }

private:
  [[nodiscard]] guard_t pin(std::uint64_t page_id, pin_mode mode) {
    cache_frame* frame = ensure_resident(page_id, mode);
    sn::util::log::ensure(frame != nullptr, "cache invariant");
    block_t* base = frame->base;
    const bool exclusive = (mode == pin_mode::exclusive);
    return guard_t(frame, base, cfg_.blocks_per_page, exclusive);
  }

  cache_frame* try_get_resident(std::uint64_t page) {
    sn::threads::lock_guard map_lock(map_mtx_);
    auto it = page_table_.find(page);
    if (it == page_table_.end()) {
      return nullptr;
    }
    cache_frame* frame = &frames_[it->second];
    if (frame->st == cache_frame::state::resident) {
      return frame;
    }
    return nullptr;
  }

  cache_frame* ensure_resident(std::uint64_t page, pin_mode mode) {
    while (true) {
      {
        sn::threads::unique_lock<sn::threads::mutex> map_lock(map_mtx_);
        auto it = page_table_.find(page);
        if (it != page_table_.end()) {
          const std::uint32_t frame_id = it->second;
          cache_frame* frame = &frames_[frame_id];
          typename cache_frame::state st = frame->st.load(std::memory_order_acquire);
          if (st == cache_frame::state::resident) {
            add_ref(frame, mode);
            map_lock.unlock();
            policy_.touch(frame_id);
            stats_.hits.fetch_add(1U, std::memory_order_relaxed);
            return frame;
          }

          if (st == cache_frame::state::evicting) {
            eviction_cv_.wait(map_lock);
            continue;
          }
          map_lock.unlock();

          wait_until_ready(frame);

          continue;
        }
      }

      sn::threads::unique_lock<sn::threads::mutex> map_lock(map_mtx_);
      auto it2 = page_table_.find(page);
      if (it2 != page_table_.end()) {

        map_lock.unlock();
        continue;
      }

      const std::uint32_t frame_id = get_frame_with_eviction(map_lock, page);
      if (frame_id == k_invalid_frame) {
        continue;
      }

      cache_frame* frame = &frames_[frame_id];
      frame->page_id = page;
      frame->st.store(cache_frame::state::loading, std::memory_order_release);
      frame->dirty.store(false, std::memory_order_relaxed);
      if (mode == pin_mode::exclusive) {
        frame->refs.set_first_exclusive();
      } else {
        frame->refs.set_first_shared();
      }

      frame_rollback rollback{this, frame, frame_id};
      page_table_.emplace(page, frame_id);
      rollback.commit();

      update_peak_resident();
      map_lock.unlock();

      try {
        backend_->read_page(page, frame->base, page_bytes_);
      } catch (...) {
        sn::threads::lock_guard lock(map_mtx_);
        recycle_frame(frame_id, page);
        throw;
      }
      stats_.misses.fetch_add(1U, std::memory_order_relaxed);
      stats_.loads.fetch_add(1U, std::memory_order_relaxed);
      stats_.read_ios.fetch_add(1U, std::memory_order_relaxed);
      stats_.read_bytes.fetch_add(page_bytes_, std::memory_order_relaxed);

      {
        sn::threads::lock_guard lock(map_mtx_);
        frame->st.store(cache_frame::state::resident, std::memory_order_release);
        state_cv_.notify_all();
      }
      policy_.touch(frame_id);
      return frame;
    }
  }

  cache_frame* ensure_new_no_load(std::uint64_t page) {
    while (true) {
      sn::threads::unique_lock<sn::threads::mutex> map_lock(map_mtx_);
      auto it = page_table_.find(page);
      if (it != page_table_.end()) {
        map_lock.unlock();
        return ensure_resident(page, pin_mode::exclusive);
      }

      const std::uint32_t frame_id = get_frame_with_eviction(map_lock, page);
      if (frame_id == k_invalid_frame) {
        continue;
      }

      cache_frame* frame = &frames_[frame_id];
      frame->page_id = page;
      frame->st.store(cache_frame::state::loading, std::memory_order_release);
      frame->dirty.store(false, std::memory_order_relaxed);
      frame->refs.set_first_exclusive();

      frame_rollback rollback{this, frame, frame_id};
      page_table_.emplace(page, frame_id);
      rollback.commit();

      update_peak_resident();
      map_lock.unlock();

      stats_.misses.fetch_add(1U, std::memory_order_relaxed);
      stats_.loads.fetch_add(1U, std::memory_order_relaxed);

      {
        sn::threads::lock_guard lock(map_mtx_);
        frame->st.store(cache_frame::state::resident, std::memory_order_release);
        state_cv_.notify_all();
      }
      policy_.touch(frame_id);
      return frame;
    }
  }

  [[nodiscard]] guard_t pin_no_load(std::uint64_t page_id) {
    cache_frame* frame = ensure_new_no_load(page_id);
    block_t* base = frame->base;
    return guard_t(frame, base, cfg_.blocks_per_page, true);
  }

  void add_ref(cache_frame* frame, pin_mode mode) {
    if (mode == pin_mode::exclusive) {
      frame->refs.acquire_exclusive();
    } else {
      frame->refs.add_shared();
    }
  }

  void wait_until_ready(cache_frame* frame) {
    sn::threads::unique_lock<sn::threads::mutex> lock(map_mtx_);
    while (true) {
      auto st = frame->st.load(std::memory_order_acquire);
      if (st == cache_frame::state::resident || st == cache_frame::state::free) {
        return;
      }
      state_cv_.wait(lock);
    }
  }

  static constexpr std::uint32_t k_invalid_frame = std::numeric_limits<std::uint32_t>::max();

  std::uint32_t get_frame_with_eviction(
      sn::threads::unique_lock<sn::threads::mutex>& map_lock, std::uint64_t target_page
  ) {

    if (!free_frame_indices_.empty()) {
      std::uint32_t i = free_frame_indices_.back();
      free_frame_indices_.pop_back();
      policy_.add(i);
      return i;
    }

    while (true) {
      const std::uint32_t victim = policy_.choose_victim();
      sn::util::log::ensure(victim != cache::lru_policy::k_invalid, "cache invariant");
      cache_frame& frame = frames_[victim];
      auto st = frame.st.load(std::memory_order_acquire);

      if (st == cache_frame::state::evicting) {
        eviction_cv_.wait(map_lock);
        continue;
      }

      if (st != cache_frame::state::resident || !frame.refs.idle()) {
        policy_.touch(victim);
        map_lock.unlock();
        sn::threads::thread_yield();
        map_lock.lock();
        continue;
      }

      const std::uint64_t victim_page = frame.page_id;
      const bool victim_dirty = frame.dirty.load(std::memory_order_acquire);
      frame.st.store(cache_frame::state::evicting, std::memory_order_release);

      map_lock.unlock();
      try {
        if (victim_dirty) {
          backend_->write_page(victim_page, frame.base, page_bytes_);
          stats_.write_ios.fetch_add(1U, std::memory_order_relaxed);
          stats_.write_bytes.fetch_add(page_bytes_, std::memory_order_relaxed);
          stats_.writebacks.fetch_add(1U, std::memory_order_relaxed);
          stats_.dirty_evictions.fetch_add(1U, std::memory_order_relaxed);
        } else {
          stats_.clean_evictions.fetch_add(1U, std::memory_order_relaxed);
        }
      } catch (...) {
        map_lock.lock();
        recycle_frame(victim, victim_page);
        throw;
      }
      eviction_count_.fetch_add(1ULL, std::memory_order_relaxed);
      stats_.evictions.fetch_add(1U, std::memory_order_relaxed);

      map_lock.lock();

      cache_frame::reset_state(frame);

      frame.st.store(cache_frame::state::free, std::memory_order_release);

      auto it = page_table_.find(victim_page);
      if (it != page_table_.end()) {
        page_table_.erase(it);
      }

      eviction_cv_.notify_all();
      state_cv_.notify_all();

      if (page_table_.find(target_page) != page_table_.end()) {

        free_frame_indices_.push_back(victim);
        return k_invalid_frame;
      }

      policy_.touch(victim);
      return victim;
    }
  }

  void update_peak_resident() {
    const std::uint64_t current = static_cast<std::uint64_t>(page_table_.size());
    std::uint64_t prev = stats_.peak_resident.load(std::memory_order_relaxed);
    while (current > prev && !stats_.peak_resident.compare_exchange_weak(
                                 prev, current, std::memory_order_relaxed, std::memory_order_relaxed
                             )) {

    }
  }

  void recycle_frame(std::uint32_t frame_id, std::uint64_t page_id) {
    cache_frame& frame = frames_[frame_id];
    cache_frame::reset_state(frame);

    frame.st.store(cache_frame::state::free, std::memory_order_release);

    auto it = page_table_.find(page_id);
    if (it != page_table_.end() && it->second == frame_id) {
      page_table_.erase(it);
    }

    free_frame_indices_.push_back(frame_id);
    state_cv_.notify_all();
    eviction_cv_.notify_all();
  }

  config cfg_{};
  std::unique_ptr<io::backend> backend_{};
  std::vector<cache_frame> frames_;
  struct aligned_deleter {
    std::size_t alignment = alignof(block_t);
    void operator()(block_t* ptr) const noexcept { ::operator delete[](ptr, std::align_val_t(alignment)); }
  };

  static std::unique_ptr<block_t[], aligned_deleter> allocate_aligned(std::size_t count, std::size_t alignment) {
    const std::size_t bytes = count * sizeof(block_t);
    void* mem = ::operator new[](bytes, std::align_val_t(alignment));
    return std::unique_ptr<block_t[], aligned_deleter>(static_cast<block_t*>(mem), aligned_deleter{alignment});
  }

  std::unique_ptr<block_t[], aligned_deleter> backing_;
  std::unordered_map<std::uint64_t, std::uint32_t> page_table_;
  sn::threads::mutex map_mtx_;
  cache::lru_policy policy_{};
  std::size_t page_blocks_ = 0;
  std::size_t page_bytes_ = 0;
  std::atomic<std::uint64_t> eviction_count_{0};
  cache::stats stats_{};
  std::vector<std::uint32_t> free_frame_indices_;
  sn::threads::condition_variable eviction_cv_;
  sn::threads::condition_variable state_cv_;

  struct frame_rollback {
    cache_manager* mgr;
    cache_frame* frame;
    std::uint32_t frame_id;
    bool committed = false;

    ~frame_rollback() {
      if (!committed) {
        cache_frame::reset_state(*frame);
        frame->st.store(cache_frame::state::free, std::memory_order_release);
        mgr->free_frame_indices_.push_back(frame_id);
        mgr->state_cv_.notify_all();
        mgr->eviction_cv_.notify_all();
      }
    }

    void commit() { committed = true; }
  };
};

}
