#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/util/log.hpp"

namespace sn::omap::pmchain::util {

inline std::uint64_t round_up_to_multiple_u64(std::uint64_t value, std::uint64_t multiple) {
  if (multiple == 0 || value == 0) {
    return std::max(value, multiple);
  }
  const std::uint64_t remainder = value % multiple;
  if (remainder == 0) {
    return value;
  }
  const std::uint64_t delta = multiple - remainder;
  sn::util::log::ensure(
      value <= std::numeric_limits<std::uint64_t>::max() - delta, "round_up_to_multiple_u64: overflow"
  );
  return value + delta;
}

struct zingoram_config_input {
  std::size_t block_count = 0;
  std::size_t batch_size = 0;
  std::uint32_t bucket_real_size = 0;
  std::uint32_t bucket_dummy_size = 0;
  std::uint32_t routing_depth = 0;
  std::uint32_t evict_batch = 1;
  std::uint32_t access_concurrency = 1;
  std::uint64_t disjoint_epoch_window = 0;
};

template <typename Traits> struct zingoram_setup_result {
  typename Traits::options_t opts{};
  std::uint64_t disjoint_window = 0;
  std::uint64_t computed_eviction_rate = 0;
  std::uint64_t pathreads_per_batch = 0;
  std::uint64_t num_pathreads = 0;
};

template <typename Traits>
inline zingoram_setup_result<Traits> compute_zingoram_setup(
    const zingoram_config_input& cfg, std::string_view label = "zingoram"
) {
  zingoram_setup_result<Traits> result{};
  const std::string prefix(label);

  const std::uint64_t computed_eviction_rate =
      sn::oram::zingoram::analysis::max_eviction_rate(cfg.bucket_real_size);
  sn::util::log::ensure(
      computed_eviction_rate <= std::numeric_limits<std::uint32_t>::max(),
      prefix + ": eviction rate exceeds 32-bit limit"
  );

  const std::uint64_t subtree_count = static_cast<std::uint64_t>(sn::oram::zingoram::subtree_count(cfg.routing_depth));
  sn::util::log::ensure(
      subtree_count <=
          std::numeric_limits<std::uint64_t>::max() / std::max<std::uint64_t>(computed_eviction_rate, 1ULL),
      prefix + ": disjoint epoch subtree overflow"
  );
  const std::uint64_t pathreads_per_batch = computed_eviction_rate * subtree_count;
  const std::uint32_t evict_batch = std::max<std::uint32_t>(cfg.evict_batch, 1U);
  sn::util::log::ensure(
      pathreads_per_batch <= std::numeric_limits<std::uint64_t>::max() / evict_batch,
      prefix + ": disjoint epoch window overflow"
  );
  const std::uint64_t num_pathreads = pathreads_per_batch * static_cast<std::uint64_t>(evict_batch);

  const std::uint64_t min_window = round_up_to_multiple_u64(
      std::max<std::uint64_t>(cfg.batch_size, num_pathreads), std::max<std::uint64_t>(num_pathreads, 1ULL)
  );
  std::uint64_t disjoint_window = cfg.disjoint_epoch_window;
  if (disjoint_window == 0) {
    disjoint_window = min_window;
  } else {
    const std::uint64_t clamped = std::max<std::uint64_t>(disjoint_window, cfg.batch_size);
    disjoint_window = round_up_to_multiple_u64(clamped, std::max<std::uint64_t>(num_pathreads, 1ULL));
  }

  typename Traits::options_t opts{};
  opts.block_count = cfg.block_count;
  opts.bucket_real_size = cfg.bucket_real_size;
  opts.bucket_dummy_size = cfg.bucket_dummy_size;
  opts.eviction_rate = static_cast<std::uint32_t>(computed_eviction_rate);
  opts.routing_depth = cfg.routing_depth;
  opts.evict_batch = evict_batch;
  opts.access_concurrency = std::max<std::uint32_t>(cfg.access_concurrency, 1U);
  opts.disjoint_epoch_window = disjoint_window;

  result.opts = opts;
  result.disjoint_window = disjoint_window;
  result.computed_eviction_rate = computed_eviction_rate;
  result.pathreads_per_batch = pathreads_per_batch;
  result.num_pathreads = num_pathreads;
  return result;
}

} // namespace sn::omap::pmchain::util
