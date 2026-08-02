#pragma once

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>
#include <string>

#include "sonic/util/humanize.hpp"
#include "sonic/util/log.hpp"

#include "sonic/oram/storage/block_heap.hpp"
#include "sonic/oram/storage/layout_planner.hpp"
#include "sonic/oram/storage/tiered_store.hpp"
#include "sonic/oram/storage/slab_store.hpp"
#include "sonic/oram/tree/topology.hpp"

namespace sn::oram::zingoram {

// block storage allocator: owns the hot slab, and computes hot/cold split
// hot slab stays resident in memory; cold buckets are stored externally
template <typename Block, typename Store> class allocator {
public:
  using block_t = Block;
  using store_t = Store;

  struct plan {
    storage::hot_layout_plan hot{};
    std::uint32_t slot_count = 0;
    std::size_t n_nodes_inclusive = 0; // node_count + 1
    std::uint64_t bucket_bytes = 0;
    struct cache_layout {
      bool block_granular = false;
      std::uint32_t levels_per_pack = 1;   // bucket mode: levels per page
      std::uint32_t block_pack_factor = 1; // block mode: blocks per page
      std::uint32_t cold_start_level = 0;
      std::uint32_t cold_level_max = 0;
      std::uint32_t buckets_per_page = 1;
      std::uint32_t blocks_per_page = 0;
      std::uint32_t pages_per_bucket = 1;
      std::uint32_t frame_count = 0;
      std::uint64_t page_bytes = 0;
      std::uint64_t expected_pages = 0;
    } cache{};
  };

  using cache_plan = typename plan::cache_layout;

  allocator(
      const sn::oram::tree::topology& /*topo*/, std::uint64_t node_count, std::uint64_t height,
      std::uint32_t slot_count, std::uint64_t hot_budget_bytes, std::uint32_t cache_pack_factor,
      std::uint64_t cache_budget_bytes, std::uint32_t access_concurrency, sn::util::log::logger& log
  ) :
      logger_(log), access_concurrency_(access_concurrency ? access_concurrency : 1) {
    // number of blocks per bucket
    plan_.slot_count = slot_count;
    // root is bucket 1; bucket 0 is sentinel
    plan_.n_nodes_inclusive = static_cast<std::size_t>(node_count + 1ULL);
    // total size of a bucket's blocks
    plan_.bucket_bytes = static_cast<std::uint64_t>(slot_count) * sizeof(block_t);

    configure_hot_plan(node_count, height, hot_budget_bytes);
    configure_cache_plan(node_count, height, cache_pack_factor, cache_budget_bytes);
    log_plan();
    wire_bucket_ptrs(node_count);
  }

  const plan& layout() const noexcept { return plan_; }

  const std::vector<block_t*>& bucket_ptrs() const noexcept { return bucket_ptrs_; }

  void log_plan() const {
    const std::uint64_t total_nodes = plan_.n_nodes_inclusive - 1ULL;
    const std::uint64_t total_bytes = total_nodes * plan_.bucket_bytes;

    if constexpr (is_tiered) {
      log_tiered_plan(total_nodes, total_bytes);
    } else {
      log_slab_plan(total_nodes, total_bytes);
    }
  }

private:
  static constexpr bool is_tiered = std::is_same_v<store_t, sn::oram::zingoram::storage::tiered_store<Block>>;
  static constexpr std::uint32_t k_min_concurrency = 1;

  void configure_hot_plan(std::uint64_t node_count, std::uint64_t height, std::uint64_t budget_bytes) {
    budget_bytes_ = budget_bytes;
    if constexpr (is_tiered) {
      plan_.hot = storage::plan_hot_layout(height, plan_.bucket_bytes, budget_bytes_);
    } else {
      // non-tiered: everything is hot
      plan_.hot.hot_levels = static_cast<std::uint32_t>(height + 1ULL);
      plan_.hot.hot_level_max = static_cast<std::int64_t>(height);
      plan_.hot.hot_node_count = node_count;
      plan_.hot.hot_last_node_id = node_count;
    }
  }

  void configure_cache_plan(
      std::uint64_t node_count, std::uint64_t height, std::uint32_t cache_pack_factor, std::uint64_t cache_budget_bytes
  ) {
    if constexpr (!is_tiered) {
      (void) node_count;
      (void) cache_pack_factor;
      (void) cache_budget_bytes;
      return;
    }
    sn::util::log::ensure(cache_pack_factor > 0, "zingoram::allocator: cache_pack_factor must be positive");
    sn::util::log::ensure(cache_budget_bytes > 0, "zingoram::allocator: cache_budget_bytes must be positive");
    cache_budget_bytes_ = cache_budget_bytes;

    auto& cache = plan_.cache;
    cache.block_granular = should_use_block_cache();
    cache.levels_per_pack = cache_pack_factor;   // used when in bucket mode
    cache.block_pack_factor = cache_pack_factor; // used when in block mode
    cache.cold_start_level = plan_.hot.hot_levels;
    cache.cold_level_max = static_cast<std::uint32_t>(height);

    configure_page_geometry(cache);

    const std::uint64_t frames_by_budget = cache_budget_bytes / cache.page_bytes;
    sn::util::log::ensure(frames_by_budget > 0, "zingoram::allocator: cache budget too small for one page");

    const std::uint64_t cold_nodes = node_count - plan_.hot.hot_node_count;
    if (cold_nodes == 0) {
      cache.frame_count = 1;
      cache.expected_pages = 0;
      return;
    }

    if (cache.block_granular) {
      configure_block_cache(cache, frames_by_budget, cold_nodes);
    } else {
      configure_bucket_cache(cache, frames_by_budget, height);
    }
  }

  void wire_bucket_ptrs(std::uint64_t node_count) {
    bucket_ptrs_.assign(plan_.n_nodes_inclusive, nullptr);

    // no hot nodes: nothing to do
    if (plan_.hot.hot_node_count == 0) {
      return;
    }

    const std::size_t heap_nodes = plan_.hot.hot_last_node_id + 1ULL; // include sentinel 0
    // allocate a large contiguous slab for blocks of all hot buckets
    hot_heap_.configure(heap_nodes, static_cast<std::size_t>(plan_.slot_count));

    // store bucket pointers into the slab
    for (std::uint64_t node_id = 1; node_id <= plan_.hot.hot_last_node_id; ++node_id) {
      bucket_ptrs_[node_id] = hot_heap_.base_for(static_cast<std::size_t>(node_id));
    }

    (void) node_count; // silence unused; in case all nodes are hot
  }

  plan plan_{};
  std::uint64_t budget_bytes_ = 0;
  std::uint64_t cache_budget_bytes_ = 0;
  sn::oram::zingoram::storage::block_heap<block_t> hot_heap_{};
  std::vector<block_t*> bucket_ptrs_{};
  sn::util::log::logger& logger_;
  std::uint32_t access_concurrency_ = 1;

  bool should_use_block_cache() const noexcept {
    return false;
  }

  static std::string format_level_range(std::int64_t lo, std::int64_t hi) {
    return (lo == hi) ? std::to_string(lo) : (std::to_string(lo) + "-" + std::to_string(hi));
  }

  void log_slab_plan(std::uint64_t total_nodes, std::uint64_t total_bytes) const {
    logger_.trcf(
        "zingoram::allocator: mode=slab bucket=%s slots=%u nodes=%llu total=%s",
        sn::util::humanize::bytes(plan_.bucket_bytes), plan_.slot_count, static_cast<unsigned long long>(total_nodes),
        sn::util::humanize::bytes(total_bytes)
    );
  }

  void log_tiered_plan(std::uint64_t total_nodes, std::uint64_t total_bytes) const {
    const auto& c = plan_.cache;
    const std::uint64_t hot_bytes = plan_.hot.hot_node_count * plan_.bucket_bytes;
    const std::uint64_t cold_nodes = total_nodes - plan_.hot.hot_node_count;
    const std::uint64_t cold_bytes = cold_nodes * plan_.bucket_bytes;
    const unsigned hot_pct = static_cast<unsigned>(100ULL * plan_.hot.hot_node_count / total_nodes);
    const unsigned cold_pct = 100U - hot_pct;
    const std::string budget_str = budget_bytes_ == 0 ? "unlimited" : sn::util::humanize::bytes(budget_bytes_);
    const std::string hot_range = format_level_range(0, plan_.hot.hot_level_max);
    const std::string cold_range = format_level_range(c.cold_start_level, c.cold_level_max);

    logger_.trcf(
        "zingoram::allocator: mode=tiered bucket=%s slots=%u nodes=%llu total=%s hot_budget=%s "
        "hot(levels=%s nodes=%llu bytes=%s %u%%) cold(levels=%s nodes=%llu bytes=%s %u%%) %s",
        sn::util::humanize::bytes(plan_.bucket_bytes), plan_.slot_count, static_cast<unsigned long long>(total_nodes),
        sn::util::humanize::bytes(total_bytes), budget_str, hot_range,
        static_cast<unsigned long long>(plan_.hot.hot_node_count), sn::util::humanize::bytes(hot_bytes), hot_pct,
        cold_range, static_cast<unsigned long long>(cold_nodes), sn::util::humanize::bytes(cold_bytes), cold_pct,
        cache_summary().c_str()
    );
  }

  std::string cache_summary() const {
    const auto& c = plan_.cache;
    const std::uint64_t frames_bytes = static_cast<std::uint64_t>(c.frame_count) * c.page_bytes;
    const std::uint64_t pages_bytes = c.expected_pages * c.page_bytes;
    const std::uint64_t used_bytes = static_cast<std::uint64_t>(c.frame_count) * c.page_bytes;

    if (c.block_granular) {
      return std::string("cache(mode=block pack_factor=") + std::to_string(c.block_pack_factor) +
             " blocks/page=" + std::to_string(c.blocks_per_page) +
             " pages/bucket=" + std::to_string(c.pages_per_bucket) +
             " page=" + sn::util::humanize::bytes(c.page_bytes) + " frames=" + std::to_string(c.frame_count) + "=" +
             sn::util::humanize::bytes(frames_bytes) +
             " pages=" + std::to_string(static_cast<unsigned long long>(c.expected_pages)) + "=" +
             sn::util::humanize::bytes(pages_bytes) + " used=" + sn::util::humanize::bytes(used_bytes) + ")";
    }

    return std::string("cache(mode=bucket pack_levels=") + std::to_string(c.levels_per_pack) +
           " buckets/page=" + std::to_string(c.buckets_per_page) + " blocks/page=" + std::to_string(c.blocks_per_page) +
           " page=" + sn::util::humanize::bytes(c.page_bytes) + " frames=" + std::to_string(c.frame_count) + "=" +
           sn::util::humanize::bytes(frames_bytes) +
           " pages=" + std::to_string(static_cast<unsigned long long>(c.expected_pages)) + "=" +
           sn::util::humanize::bytes(pages_bytes) + " used=" + sn::util::humanize::bytes(used_bytes) + ")";
  }

  void configure_page_geometry(cache_plan& cache) const {
    if (cache.block_granular) {
      set_block_page_geometry(cache);
    } else {
      set_bucket_page_geometry(cache);
    }
    sn::util::log::ensure(cache.blocks_per_page > 0, "zingoram::allocator: cache blocks_per_page must be positive");
    sn::util::log::ensure(cache.pages_per_bucket > 0, "zingoram::allocator: cache pages_per_bucket must be positive");
    sn::util::log::ensure(cache.page_bytes > 0, "zingoram::allocator: cache page size must be positive");
  }

  void set_block_page_geometry(cache_plan& cache) const {
    cache.buckets_per_page = 1;
    cache.blocks_per_page = cache.block_pack_factor;
    cache.pages_per_bucket = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(plan_.slot_count) + cache.blocks_per_page - 1ULL) / cache.blocks_per_page
    );
    cache.page_bytes = static_cast<std::uint64_t>(cache.blocks_per_page) * sizeof(block_t);
  }

  void set_bucket_page_geometry(cache_plan& cache) const {
    cache.buckets_per_page = (std::uint32_t(1) << cache.levels_per_pack) - 1U;
    cache.block_pack_factor = 1;
    cache.blocks_per_page = plan_.slot_count * cache.buckets_per_page;
    cache.pages_per_bucket = 1;
    cache.page_bytes = static_cast<std::uint64_t>(cache.blocks_per_page) * sizeof(block_t);
  }

  void configure_block_cache(cache_plan& cache, std::uint64_t frames_by_budget, std::uint64_t cold_nodes) const {
    const std::uint64_t pages_per_bucket = cache.pages_per_bucket;
    const std::uint64_t min_frames_block =
        pages_per_bucket * static_cast<std::uint64_t>(std::max<std::uint32_t>(k_min_concurrency, access_concurrency_));

    sn::util::log::ensure(
        frames_by_budget >= min_frames_block,
        "zingoram::allocator: cache budget insufficient for block-granular cold storage "
        "(need pages_per_bucket*concurrency frames)"
    );

    cache.expected_pages = cold_nodes * pages_per_bucket;
    cache.frame_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(cache.expected_pages, frames_by_budget));
  }

  void configure_bucket_cache(cache_plan& cache, std::uint64_t frames_by_budget, std::uint64_t height) const {
    cache.expected_pages = triangle_pages(height, cache.cold_start_level, cache.levels_per_pack);
    cache.frame_count = static_cast<std::uint32_t>(std::min<std::uint64_t>(cache.expected_pages, frames_by_budget));
  }

  static std::uint64_t triangle_pages(
      std::uint64_t height, std::uint32_t cold_start_level, std::uint32_t levels_per_pack
  ) {
    const std::uint64_t cold_levels = (height + 1ULL > cold_start_level) ? (height + 1ULL - cold_start_level) : 0ULL;
    const std::uint64_t tri_levels = (cold_levels + static_cast<std::uint64_t>(levels_per_pack) - 1ULL) /
                                     static_cast<std::uint64_t>(levels_per_pack);

    std::uint64_t triangles = 0;
    std::uint64_t term = (cold_start_level < 64) ? (1ULL << cold_start_level) : 0ULL;
    for (std::uint64_t r = 0; r < tri_levels; ++r) {
      triangles += term;
      term <<= levels_per_pack;
    }
    return triangles;
  }
};

} // namespace sn::oram::zingoram
