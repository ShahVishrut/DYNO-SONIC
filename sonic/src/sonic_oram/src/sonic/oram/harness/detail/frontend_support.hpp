#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/oram/zingoram/threading.hpp"
#include "sonic/threads/thread_pool.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::oram::harness::detail {

[[nodiscard]] inline run_options make_harness_run_options(
    std::size_t accesses, std::optional<std::size_t> disjoint_window, sn::threads::thread_pool* access_pool,
    std::size_t access_workers, bool online_only
) {
  run_options run{};
  run.access_count = accesses;
  run.mode = online_only ? run_mode::disjoint_online_only
                         : (disjoint_window.has_value() ? run_mode::disjoint_windowed : run_mode::standard);
  if (!online_only && disjoint_window.has_value()) {
    run.window_size = disjoint_window.value();
  }
  if (access_pool != nullptr) {
    run.workers.pool = access_pool;
    run.workers.max_workers = access_workers;
  }
  return run;
}

inline void await_profiler_if_needed(sn::util::log::logger&) {}

template <typename options_t>
[[nodiscard]] inline std::optional<std::size_t> try_compute_zingoram_online_only_window(
    const options_t& opts, std::size_t accesses
) noexcept {
  const std::size_t total_operations = accesses != 0 ? accesses : static_cast<std::size_t>(opts.block_count);
  const std::uint64_t base_eviction_rate =
      opts.eviction_rate != 0 ? static_cast<std::uint64_t>(opts.eviction_rate)
                              : sn::oram::zingoram::analysis::max_eviction_rate(opts.bucket_real_size);
  const std::uint64_t subtree_count = static_cast<std::uint64_t>(sn::oram::zingoram::subtree_count(opts.routing_depth));
  const std::uint64_t batch_factor = subtree_count * static_cast<std::uint64_t>(opts.evict_batch);
  const std::uint64_t num_pathreads = base_eviction_rate * batch_factor;
  if (num_pathreads == 0) {
    return std::nullopt;
  }

  const std::uint64_t min_window = static_cast<std::uint64_t>(total_operations) + 1ULL;
  if (min_window > std::numeric_limits<std::uint64_t>::max() - (num_pathreads - 1ULL)) {
    return std::nullopt;
  }

  const std::uint64_t rounded = ((min_window + num_pathreads - 1ULL) / num_pathreads) * num_pathreads;
  if (rounded > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }

  return static_cast<std::size_t>(rounded);
}

} // namespace sn::oram::harness::detail
