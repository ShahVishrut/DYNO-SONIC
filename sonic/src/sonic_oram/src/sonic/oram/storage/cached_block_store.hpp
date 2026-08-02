#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/storage/pin_mode.hpp"
#include "sonic/storage/cache_manager.hpp"

namespace sn::oram::zingoram::storage {

// block-granular store: each bucket slot is its own page
template <typename Block> class cached_block_store {
public:
  using block_t = Block;
  using manager_t = sn::storage::cache_manager<Block>;
  using manager_guard = typename manager_t::guard_t;

  class guard_t {
  public:
    guard_t() = default;

    guard_t(
        std::vector<manager_guard>&& guards, std::vector<block_t>&& staging, bool exclusive,
        std::uint32_t blocks_per_page, std::uint32_t slots
    ) :
        guards_(std::move(guards)),
        staging_(std::move(staging)),
        blocks_per_page_(blocks_per_page),
        slots_(slots),
        exclusive_(exclusive) {}

    guard_t(const guard_t&) = delete;
    guard_t& operator=(const guard_t&) = delete;
    guard_t(guard_t&&) noexcept = default;
    guard_t& operator=(guard_t&&) noexcept = default;

    ~guard_t() { flush(); }

    [[nodiscard]] sn::util::span<block_t> span() noexcept {
      return sn::util::span<block_t>(staging_.data(), staging_.size());
    }
    [[nodiscard]] sn::util::span<block_t> span() const noexcept {
      return sn::util::span<block_t>(const_cast<block_t*>(staging_.data()), staging_.size());
    }
    [[nodiscard]] block_t* data() noexcept { return staging_.data(); }
    [[nodiscard]] block_t* data() const noexcept { return const_cast<block_t*>(staging_.data()); }
    [[nodiscard]] std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(staging_.size()); }

    void mark_dirty() noexcept { dirty_ = true; }

  private:
    void flush() noexcept {
      if (!exclusive_ || !dirty_) {
        return;
      }
      // scatter back into pinned pages
      for (std::size_t page_ix = 0; page_ix < guards_.size(); ++page_ix) {
        auto& g = guards_[page_ix];
        block_t* dst = g.data();
        const std::uint32_t start = static_cast<std::uint32_t>(page_ix) * blocks_per_page_;
        const std::uint32_t end = std::min<std::uint32_t>(slots_, start + blocks_per_page_);
        for (std::uint32_t i = start, local = 0; i < end; ++i, ++local) {
          dst[local] = staging_[i];
        }
        g.mark_dirty();
      }
      dirty_ = false;
    }

    std::vector<manager_guard> guards_{};
    std::vector<block_t> staging_{};
    std::uint32_t blocks_per_page_ = 1;
    std::uint32_t slots_ = 0;
    bool exclusive_ = false;
    bool dirty_ = false;
  };

  static constexpr bool is_trivial = false;
  static constexpr bool contiguous = false;

  void configure(
      manager_t* mgr, std::uint64_t node_id, std::uint32_t slots_per_bucket, std::uint32_t blocks_per_page
  ) noexcept {
    sn::util::log::ensure(slots_per_bucket > 0, "cached_block_store: slots_per_bucket must be positive");
    mgr_ = mgr;
    node_id_ = node_id;
    slots_ = slots_per_bucket;
    blocks_per_page_ = blocks_per_page ? blocks_per_page : 1;
    pages_per_bucket_ =
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(slots_) + blocks_per_page_ - 1ULL) / blocks_per_page_);
  }

  [[nodiscard]] guard_t pin(pin_mode mode) const {
    if (mgr_ == nullptr) {
      return guard_t{};
    }
    sn::util::log::ensure(slots_ > 0, "cached_block_store: pin called before configure");
    const bool exclusive = (mode == pin_mode::exclusive);
    std::vector<manager_guard> guards;
    guards.reserve(pages_per_bucket_);
    std::vector<block_t> staging(static_cast<std::size_t>(slots_));

    for (std::uint32_t page_ix = 0; page_ix < pages_per_bucket_; ++page_ix) {
      const std::uint64_t page_id = page_for_page_index(page_ix);
      manager_guard g = exclusive ? mgr_->pin_exclusive_no_load(page_id) : mgr_->pin_shared(page_id);
      if (!exclusive) {
        const std::uint32_t start = page_ix * blocks_per_page_;
        const std::uint32_t end = std::min<std::uint32_t>(slots_, start + blocks_per_page_);
        block_t* src = g.data();
        for (std::uint32_t i = start, local = 0; i < end; ++i, ++local) {
          staging[i] = src[local];
        }
      }
      guards.emplace_back(std::move(g));
    }

    return guard_t(std::move(guards), std::move(staging), exclusive, blocks_per_page_, slots_);
  }

  [[nodiscard]] block_t read_block(std::uint32_t offset) const {
    sn::util::log::ensure(offset < slots_, "cached_block_store: read_block offset out of range");
    const std::uint64_t page_id = page_for_slot(offset);
    auto g = mgr_->pin_shared(page_id);
    return g.data()[slot_offset(offset)];
  }

  void prefetch() const noexcept {}

private:
  [[nodiscard]] std::uint64_t page_for_slot(std::uint32_t offset) const noexcept {
    return node_id_ * static_cast<std::uint64_t>(pages_per_bucket_) +
           static_cast<std::uint64_t>(offset / blocks_per_page_);
  }

  [[nodiscard]] std::uint64_t page_for_page_index(std::uint32_t page_ix) const noexcept {
    return node_id_ * static_cast<std::uint64_t>(pages_per_bucket_) + static_cast<std::uint64_t>(page_ix);
  }

  [[nodiscard]] std::uint32_t slot_offset(std::uint32_t offset) const noexcept { return offset % blocks_per_page_; }

  manager_t* mgr_ = nullptr;
  std::uint64_t node_id_ = 0;
  std::uint32_t slots_ = 0;
  std::uint32_t blocks_per_page_ = 1;
  std::uint32_t pages_per_bucket_ = 1;
};

} // namespace sn::oram::zingoram::storage
