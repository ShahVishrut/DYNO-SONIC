#pragma once

#include <cstdint>
#include <string>

namespace sn::oram::zingoram {

struct options {
  std::uint64_t block_count = 0;
  std::uint32_t bucket_real_size = 0;
  std::uint32_t bucket_dummy_size = 0;
  std::uint32_t eviction_rate = 0;
  std::uint64_t stash_bound = 0;
  std::uint32_t evict_batch = 1;
  std::uint32_t routing_depth = 0;
  std::uint32_t access_concurrency = 1;
  std::uint64_t disjoint_epoch_window = 0;
  // optional (tiered): memory budget for hot tier (0 = unlimited)
  std::uint64_t hot_memory_budget_bytes = 0;
  // optional (tiered): memory budget for cold cache pages (0 = auto)
  std::uint64_t cache_memory_budget_bytes = 0;
  // optional (tiered): page cache in front of backend (0 = disable)
  std::uint64_t backend_cache_budget_bytes = 0;
  // optional (tiered): bucket mode = pack levels per page, block mode = blocks per page
  std::uint32_t cache_pack_factor = 1;
  // optional, cache path for tiered store: must be non-empty if used
  std::string cache_path{};
};

} // namespace sn::oram::zingoram
