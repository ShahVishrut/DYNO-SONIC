#pragma once

#include <cstdint>
#include <utility>
#include <new>

#include "sonic/util/span.hpp"

#include "sonic/oram/storage/cached_bucket_store.hpp"
#include "sonic/oram/storage/cached_block_store.hpp"
#include "sonic/oram/storage/pin_mode.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/storage/cache_manager.hpp"
#include "sonic/storage/cache_manager.hpp"

namespace sn::oram::zingoram::storage {

// tiered router store: hot uses slab_store; cold uses cached bucket/block store
template <typename Block, typename PageMapper = triangle_page_mapper> class tiered_store {
public:
  using block_t = Block;
  using hot_store_t = slab_store<Block>;
  using cold_store_t = cached_bucket_store<Block, PageMapper>;
  using block_cold_store_t = cached_block_store<Block>;

  struct cold_config {
    sn::storage::cache_manager<Block>* mgr = nullptr;
    std::uint64_t node_id = 0;
    std::uint32_t len = 0;
    PageMapper mapper{};
    bool block_mode = false;
    std::uint32_t block_pack_factor = 1;
  };

  class guard_t {
  public:
    using hot_guard_t = typename hot_store_t::guard_t;
    using cold_guard_t = typename cold_store_t::guard_t;
    using block_cold_guard_t = typename block_cold_store_t::guard_t;

    enum class kind { hot, cold_bucket, cold_block };

    explicit guard_t(hot_guard_t&& g) noexcept : kind_(kind::hot) { new (&hot_) hot_guard_t(std::move(g)); }
    explicit guard_t(cold_guard_t&& g) noexcept : kind_(kind::cold_bucket) { new (&cold_) cold_guard_t(std::move(g)); }
    explicit guard_t(block_cold_guard_t&& g) noexcept : kind_(kind::cold_block) {
      new (&block_cold_) block_cold_guard_t(std::move(g));
    }

    guard_t(const guard_t&) = delete;
    guard_t& operator=(const guard_t&) = delete;

    guard_t(guard_t&& other) noexcept : kind_(other.kind_) {
      if (kind_ == kind::hot) {
        new (&hot_) hot_guard_t(std::move(other.hot_));
      } else if (kind_ == kind::cold_bucket) {
        new (&cold_) cold_guard_t(std::move(other.cold_));
      } else {
        new (&block_cold_) block_cold_guard_t(std::move(other.block_cold_));
      }
    }

    guard_t& operator=(guard_t&&) = delete;

    ~guard_t() noexcept {
      if (kind_ == kind::hot) {
        hot_.~hot_guard_t();
      } else if (kind_ == kind::cold_bucket) {
        cold_.~cold_guard_t();
      } else {
        block_cold_.~block_cold_guard_t();
      }
    }

    [[nodiscard]] sn::util::span<block_t> span() const noexcept {
      switch (kind_) {
      case kind::hot:
        return hot_.span();
      case kind::cold_bucket:
        return cold_.span();
      case kind::cold_block:
        return sn::util::span<block_t>(block_cold_.data(), block_cold_.size());
      }
      return {};
    }

    [[nodiscard]] block_t* data() const noexcept {
      switch (kind_) {
      case kind::hot:
        return hot_.data();
      case kind::cold_bucket:
        return cold_.data();
      case kind::cold_block:
        return block_cold_.data();
      }
      return nullptr;
    }
    [[nodiscard]] std::uint32_t size() const noexcept {
      switch (kind_) {
      case kind::hot:
        return hot_.size();
      case kind::cold_bucket:
        return cold_.size();
      case kind::cold_block:
        return block_cold_.size();
      }
      return 0;
    }

    void mark_dirty() noexcept {
      if (kind_ == kind::cold_bucket) {
        cold_.mark_dirty();
      } else if (kind_ == kind::cold_block) {
        block_cold_.mark_dirty();
      }
    }

  private:
    kind kind_;
    union {
      hot_guard_t hot_;
      cold_guard_t cold_;
      block_cold_guard_t block_cold_;
    };
  };

  static constexpr bool is_trivial = false;
  static constexpr bool contiguous = true;

  void configure_hot(block_t* base, std::uint32_t len) noexcept {
    is_hot_ = true;
    use_block_cold_ = false;
    hot_.configure(base, len);
  }

  void configure_cold(const cold_config& cfg) noexcept {
    is_hot_ = false;
    use_block_cold_ = cfg.block_mode;
    block_pack_factor_ = cfg.block_pack_factor ? cfg.block_pack_factor : 1;
    if (use_block_cold_) {
      block_cold_.configure(cfg.mgr, cfg.node_id, cfg.len, block_pack_factor_);
    } else {
      cold_.configure(cfg.mgr, cfg.node_id, cfg.len, cfg.mapper);
    }
  }

  [[nodiscard]] guard_t pin(pin_mode mode) const {
    if (is_hot_) {
      return guard_t{hot_.pin(mode)};
    }
    if (use_block_cold_) {
      return guard_t{block_cold_.pin(mode)};
    }
    return guard_t{cold_.pin(mode)};
  }

  [[nodiscard]] block_t read_block(std::uint32_t offset) const {
    if (is_hot_) {
      return hot_.read_block(offset);
    }
    if (use_block_cold_) {
      return block_cold_.read_block(offset);
    }
    return cold_.read_block(offset);
  }

  void prefetch() const {
    if (!is_hot_) {
      if (use_block_cold_) {
        block_cold_.prefetch();
      } else {
        cold_.prefetch();
      }
    }
  }

private:
  bool is_hot_ = true;
  bool use_block_cold_ = false;
  std::uint32_t block_pack_factor_ = 1;
  hot_store_t hot_;
  cold_store_t cold_;
  block_cold_store_t block_cold_;
};

} // namespace sn::oram::zingoram::storage
