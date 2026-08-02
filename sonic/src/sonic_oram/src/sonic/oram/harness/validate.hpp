#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/harness/detail/leaf_table.hpp"
#include "sonic/oram/harness/detail/run_plan.hpp"
#include "sonic/oram/harness/detail/shadow_state.hpp"
#include "sonic/oram/harness/detail/validate_scenarios.hpp"
#include "sonic/oram/harness/detail/workers.hpp"

namespace sn::oram::harness {

namespace detail {

constexpr std::size_t k_validate_cap = 65536;

[[nodiscard]] inline std::size_t resolve_validate_batch_access_count(
    const run_plan& plan, std::size_t requested_batch_accesses
) noexcept {
  if (requested_batch_accesses != 0) {
    return requested_batch_accesses;
  }
  return std::min<std::size_t>(static_cast<std::size_t>(plan.block_count), k_validate_cap);
}

} // namespace detail

template <typename traits_t, typename Client> validate_result validate(Client& client, const validate_options& opts) {
  auto workers = detail::resolve_worker_setup(opts.run.workers);
  const detail::run_plan plan =
      detail::resolve_run_plan(client, opts.run, detail::run_profile::validate, workers.worker_count);

  validate_result result{};
  const std::size_t iterations = std::max<std::size_t>(opts.iterations, 1);
  const std::size_t access_count = detail::resolve_validate_batch_access_count(plan, opts.batch_accesses);

  detail::seed_stream run_seeds(plan.seed);
  for (std::size_t iter = 0; iter < iterations; ++iter) {
    detail::seed_stream iter_seeds(run_seeds.next_seed());
    auto leaves =
        detail::make_random_leaf_table(static_cast<std::size_t>(plan.block_count), plan.leaf_count, iter_seeds);

    detail::run_dummy_probe<traits_t>(client, plan.leaf_count, iter_seeds, plan.close);
    result.dummy_probe_accesses += 2;

    if (!plan.commit_leaf_updates) {
      detail::run_online_only_batch_pass<traits_t>(client, plan, leaves, workers.parallel, access_count, iter_seeds);
      result.batch_accesses += access_count;
      continue;
    }

    detail::shadow_state<traits_t::block_t::byte_size> shadow(static_cast<std::size_t>(plan.block_count));

    detail::run_round_trip<traits_t>(
        client, leaves, plan.leaf_count, iter_seeds,
        plan.mode == run_mode::disjoint_windowed ? detail::window_close::flush : detail::window_close::none
    );
    result.round_trip_accesses += 2;

    detail::run_stateful_batch_validate<traits_t>(
        client, plan, leaves, shadow, workers.parallel, access_count, iter_seeds
    );
    result.batch_accesses += access_count * 2;
  }

  return result;
}

} // namespace sn::oram::harness
