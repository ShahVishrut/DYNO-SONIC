#pragma once

#include <cstdint>
#include <utility>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/storage/cache_manager.hpp"
#include "sonic/oram/storage/pin_mode.hpp"
#include "sonic/oram/storage/page_mapper.hpp"

namespace sn::oram::zingoram::storage {

// bucket-granular store; can map multiple buckets to a page
template <typename Block, typename PageMapper = consecutive_page_mapper> class cached_bucket_store {
public:
  using block_t = Block;
  using manager_t = sn::storage::cache_manager<Block>;
  using manager_guard = typename manager_t::guard_t;

  class guard_t {
  public:
    guard_t() = default;
    guard_t(manager_guard&& inner, block_t* ptr, std::uint32_t len) noexcept :
        inner_(std::move(inner)), ptr_(ptr), len_(len) {}

    guard_t(const guard_t&) = delete;
    guard_t& operator=(const guard_t&) = delete;
    guard_t(guard_t&&) noexcept = default;
    guard_t& operator=(guard_t&&) noexcept = default;
    ~guard_t() noexcept = default;

    [[nodiscard]] sn::util::span<block_t> span() const noexcept {
      return sn::util::span<block_t>(ptr_, static_cast<std::size_t>(len_));
    }
    [[nodiscard]] block_t* data() const noexcept { return ptr_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return len_; }

    void mark_dirty() noexcept { inner_.mark_dirty(); }

  private:
    manager_guard inner_{};
    block_t* ptr_ = nullptr;
    std::uint32_t len_ = 0;
  };

  static constexpr bool is_trivial = false;
  static constexpr bool contiguous = true;

  void configure(
      manager_t* mgr, std::uint64_t node_id, std::uint32_t slots_per_bucket, const PageMapper& mapper
  ) noexcept {
    mgr_ = mgr;
    node_id_ = node_id;
    slots_ = slots_per_bucket;
    mapper_ = mapper;
  }

  [[nodiscard]] guard_t pin(pin_mode mode) const {
    if (mgr_ == nullptr) {
      return guard_t{};
    }
    const std::uint64_t page_id = mapper_.page_id(node_id_);
    const std::uint32_t bucket_off = mapper_.bucket_offset(node_id_);
    const std::uint32_t slot_off = bucket_off * slots_;

    manager_guard mg = (mode == pin_mode::shared) ? mgr_->pin_shared(page_id) : mgr_->pin_exclusive(page_id);
    block_t* base = mg.data() + slot_off;
    return guard_t(std::move(mg), base, slots_);
  }

  [[nodiscard]] block_t read_block(std::uint32_t offset) const {
    sn::util::log::ensure(offset < slots_, "cached_bucket_store: read_block offset out of range");
    const std::uint64_t page_id = mapper_.page_id(node_id_);
    const std::uint32_t bucket_off = mapper_.bucket_offset(node_id_);
    const std::uint32_t slot_off = bucket_off * slots_ + offset;
    manager_guard mg = mgr_->pin_shared(page_id);
    return mg.data()[slot_off];
  }

  void prefetch() const {
    if (mgr_ != nullptr) {
      mgr_->prefetch(mapper_.page_id(node_id_));
    }
  }

private:
  manager_t* mgr_ = nullptr;
  std::uint64_t node_id_ = 0;
  std::uint32_t slots_ = 0;
  PageMapper mapper_{};
};

} // namespace sn::oram::zingoram::storage
