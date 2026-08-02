#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/harness/detail/access_stream.hpp"
#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/detail/leaf_table.hpp"
#include "sonic/oram/harness/detail/run_plan.hpp"
#include "sonic/oram/harness/detail/workers.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::harness {

namespace detail {

[[nodiscard]] inline std::size_t experiment_window_size(const run_plan& plan) noexcept {
  if (plan.mode == run_mode::standard) {
    return 0;
  }
  if (plan.mode == run_mode::disjoint_windowed) {
    return plan.requested_window_size;
  }
  return plan.requested_window_size != 0 ? plan.requested_window_size : plan.access_count;
}

template <typename traits_t, typename Client, typename Clock>
experiment_result run_experiment(
    Client& client, const run_plan& plan, leaf_table& leaves, std::optional<sn::threads::thread_team>& workers
) {
  experiment_result result{};
  result.access_count = plan.access_count;

  const std::size_t window_size = experiment_window_size(plan);
  const std::size_t worker_count = stream_worker_count(plan.concurrency, plan.access_count, window_size);
  result.concurrency = worker_count;

  seed_stream seeds(plan.seed);
  auto states = make_stream_worker_states<traits_t::block_t::byte_size>(client, worker_count, seeds);

  const auto start = Clock::now();

  // measure one access stream under the schedule and window mode
  (void) run_access_span(
      workers, worker_count, 0, plan.access_count, window_size, 0,
      [&](std::size_t worker_index, const access_coord& coord) {
        auto& state = states[worker_index];
        const std::uint64_t address = plan.schedule.address(coord);
        const std::uint64_t cur_leaf = leaves.current(address);
        const std::uint64_t new_leaf = sample_leaf(state.rng, plan.leaf_count);

        detail::issue_access(
            client, state.scratch, static_cast<std::int64_t>(address), cur_leaf, new_leaf, false, state.in_buf,
            state.out_buf
        );
        if (plan.commit_leaf_updates) {
          leaves.commit(address, new_leaf);
        }
      },
      [&](std::uint64_t) { apply_window_close(client, plan.close); }
  );

  const auto end = Clock::now();
  result.elapsed_seconds = Clock::seconds_between(start, end);
  if (result.elapsed_seconds > 0.0) {
    result.throughput_ops_per_sec = static_cast<double>(result.access_count) / result.elapsed_seconds;
    result.throughput_bytes_per_sec = result.throughput_ops_per_sec * static_cast<double>(traits_t::block_t::byte_size);
  }
  return result;
}

} // namespace detail

template <typename traits_t, typename Client, typename Clock = default_clock_traits>
experiment_result experiment(Client& client, const experiment_options& opts) {
  auto workers = detail::resolve_worker_setup(opts.run.workers);
  const detail::run_plan plan =
      detail::resolve_run_plan(client, opts.run, detail::run_profile::experiment, workers.worker_count);

  detail::seed_stream seeds(plan.seed);
  auto leaves = detail::make_random_leaf_table(static_cast<std::size_t>(plan.block_count), plan.leaf_count, seeds);
  return detail::run_experiment<traits_t, Client, Clock>(client, plan, leaves, workers.parallel);
}

} // namespace sn::oram::harness
