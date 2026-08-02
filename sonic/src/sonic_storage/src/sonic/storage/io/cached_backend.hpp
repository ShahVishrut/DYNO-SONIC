#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "sonic/util/log.hpp"

#include "sonic/storage/cache_manager.hpp"
#include "sonic/storage/io/backend.hpp"

namespace sn::storage::io {

class cached_backend : public backend {
public:
  struct config {

    std::size_t page_bytes = 0;

    std::size_t frame_bytes_budget = 0;

    std::uint32_t frame_count_override = 0;
    bool enable_prefetch = false;
  };

  cached_backend(config cfg, std::unique_ptr<backend> inner) :
      page_bytes_(cfg.page_bytes), cache_(make_cache_cfg(cfg), std::move(inner)) {}

  cached_backend(const cached_backend&) = delete;
  cached_backend& operator=(const cached_backend&) = delete;

  void read_page(std::uint64_t page_id, void* dst, std::size_t bytes) override {
    namespace log = sn::util::log;
    log::ensure(bytes == page_bytes_, "cache page");
    auto g = cache_.pin_shared(page_id);
    std::memcpy(dst, g.data(), page_bytes_);
  }

  void write_page(std::uint64_t page_id, const void* src, std::size_t bytes) override {
    namespace log = sn::util::log;
    log::ensure(bytes == page_bytes_, "cache page");
    auto g = cache_.pin_exclusive_no_load(page_id);
    std::memcpy(g.data(), src, page_bytes_);
    g.mark_dirty();
  }

  void flush() override { cache_.flush_dirty(); }

  void drop_cache() override {
    cache_.invalidate_all(true);
    if (auto* inner = cache_.backend()) {
      inner->drop_cache();
    }
  }

  void resize(std::uint64_t pages, std::size_t bytes_per_page) override {
    namespace log = sn::util::log;
    log::ensure(bytes_per_page == page_bytes_, "cache page");
    cache_.invalidate_all(true);
    if (auto* inner = cache_.backend()) {
      inner->resize(pages, bytes_per_page);
    }
  }

  [[nodiscard]] sn::storage::cache::stats_snapshot stats_snapshot() const noexcept { return cache_.stats_snapshot(); }
  void stats_reset() noexcept { cache_.stats_reset(); }
  [[nodiscard]] std::size_t page_bytes() const noexcept { return page_bytes_; }

private:
  static typename cache_manager<std::byte>::config make_cache_cfg(const config& cfg) {
    namespace log = sn::util::log;
    log::ensure(cfg.page_bytes > 0, "cache page");
    typename cache_manager<std::byte>::config out{};
    out.blocks_per_page = static_cast<std::uint32_t>(cfg.page_bytes);

    std::uint32_t frame_count = cfg.frame_count_override;
    if (frame_count == 0) {
      log::ensure(cfg.frame_bytes_budget >= cfg.page_bytes, "cache budget");
      frame_count = static_cast<std::uint32_t>(cfg.frame_bytes_budget / cfg.page_bytes);
      frame_count = frame_count == 0 ? 1U : frame_count;
    }
    out.frame_count = frame_count;
    out.enable_prefetch = cfg.enable_prefetch;
    return out;
  }

  std::size_t page_bytes_{0};
  cache_manager<std::byte> cache_;
};

}
