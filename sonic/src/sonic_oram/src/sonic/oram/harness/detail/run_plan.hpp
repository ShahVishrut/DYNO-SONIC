#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>

#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/harness/detail/access_schedule.hpp"
#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/detail/random.hpp"
#include "sonic/oram/harness/detail/workers.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::harness::detail {

enum class run_profile {
  experiment,
  benchmark_latency,
  benchmark_throughput,
  validate,
};

struct schedule_plan {
  schedule_kind kind = schedule_kind::fixed;
  schedule_scope scope = schedule_scope::stream;
  std::uint64_t block_count = 0;
  std::uint64_t fixed_address = 0;
  std::uint64_t start = 0;
  std::uint64_t step = 1;
  std::uint64_t unique_period = 1;

  [[nodiscard]] std::uint64_t address(const access_coord& coord) const noexcept {
    switch (kind) {
    case schedule_kind::fixed:
      return address_of(fixed_schedule{fixed_address}, coord);
    case schedule_kind::per_worker_fixed:
      return address_of(per_worker_fixed_schedule{block_count, start}, coord);
    case schedule_kind::round_robin:
      return address_of(round_robin_schedule{block_count, start, scope}, coord);
    case schedule_kind::affine:
      return address_of(affine_schedule{block_count, start, step, scope}, coord);
    case schedule_kind::automatic:
      break;
    }
    return 0;
  }
};

// plan for one action
struct run_plan {
  run_profile profile = run_profile::experiment;
  run_mode mode = run_mode::standard;
  std::size_t access_count = 0;
  std::size_t concurrency = 1;
  std::uint64_t block_count = 0;
  std::uint64_t leaf_count = 0;
  schedule_plan schedule{};
  std::size_t requested_window_size = 0;
  window_close close = window_close::none;
  bool requires_unique_addresses = false;
  bool commit_leaf_updates = true;
  std::uint64_t seed = 0;
};

template <typename Client> [[nodiscard]] inline bool client_supports_mode(run_mode mode) noexcept {
  switch (mode) {
  case run_mode::standard:
    return true;
  case run_mode::disjoint_windowed:
    return has_flush_epoch_v<Client>;
  case run_mode::disjoint_online_only:
    return has_drop_epoch_v<Client>;
  }
  return false;
}

[[nodiscard]] inline schedule_kind select_schedule_kind(run_profile profile, run_mode mode, schedule_kind requested) {
  if (requested != schedule_kind::automatic) {
    return requested;
  }

  switch (profile) {
  case run_profile::benchmark_latency:
    return mode == run_mode::standard ? schedule_kind::fixed : schedule_kind::affine;
  case run_profile::benchmark_throughput:
  case run_profile::experiment:
    return mode == run_mode::standard ? schedule_kind::per_worker_fixed : schedule_kind::affine;
  case run_profile::validate:
    return mode == run_mode::standard ? schedule_kind::round_robin : schedule_kind::affine;
  }
  return schedule_kind::fixed;
}

[[nodiscard]] inline schedule_scope select_schedule_scope(
    run_profile profile, run_mode mode, schedule_kind resolved_kind, schedule_scope requested
) noexcept {
  if (mode == run_mode::standard || resolved_kind == schedule_kind::fixed ||
      resolved_kind == schedule_kind::per_worker_fixed) {
    return schedule_scope::stream;
  }

  if (requested == schedule_scope::window && mode == run_mode::disjoint_online_only &&
      profile == run_profile::benchmark_throughput) {
    return schedule_scope::stream;
  }

  if (mode == run_mode::disjoint_windowed) {
    return schedule_scope::window;
  }

  return requested;
}

[[nodiscard]] inline window_close select_close(run_mode mode) noexcept {
  switch (mode) {
  case run_mode::standard:
    return window_close::none;
  case run_mode::disjoint_windowed:
    return window_close::flush;
  case run_mode::disjoint_online_only:
    return window_close::drop;
  }
  return window_close::none;
}

[[nodiscard]] inline std::uint64_t resolve_schedule_unique_period(const schedule_plan& schedule) noexcept {
  switch (schedule.kind) {
  case schedule_kind::fixed:
    return unique_period(fixed_schedule{schedule.fixed_address});
  case schedule_kind::per_worker_fixed:
    return unique_period(per_worker_fixed_schedule{schedule.block_count, schedule.start});
  case schedule_kind::round_robin:
    return unique_period(round_robin_schedule{schedule.block_count, schedule.start, schedule.scope});
  case schedule_kind::affine:
    return unique_period(affine_schedule{schedule.block_count, schedule.start, schedule.step, schedule.scope});
  case schedule_kind::automatic:
    break;
  }
  return 1;
}

template <typename Client>
[[nodiscard]] inline run_plan build_base_plan(
    const Client& client, const run_options& opts, run_profile profile, std::size_t available_workers
) {
  run_plan plan{};
  plan.profile = profile;
  plan.mode = opts.mode;
  plan.block_count = static_cast<std::uint64_t>(client.options().block_count);
  plan.leaf_count = client.shape().leaf_count;
  plan.access_count = opts.access_count != 0 ? opts.access_count : static_cast<std::size_t>(plan.block_count);
  plan.concurrency = resolve_concurrency(
      client, std::max<std::size_t>(1, available_workers), static_cast<std::size_t>(plan.block_count)
  );
  plan.seed = resolve_run_seed(opts.seed);
  plan.close = select_close(opts.mode);
  plan.requested_window_size = opts.window_size;
  plan.requires_unique_addresses = opts.mode != run_mode::standard;
  plan.commit_leaf_updates = opts.mode != run_mode::disjoint_online_only;
  return plan;
}

[[nodiscard]] inline schedule_plan resolve_schedule_plan(const run_plan& plan, const schedule_options& opts) {
  schedule_plan schedule{};
  schedule.kind = select_schedule_kind(plan.profile, plan.mode, opts.kind);
  schedule.scope = select_schedule_scope(plan.profile, plan.mode, schedule.kind, opts.scope);
  schedule.block_count = plan.block_count;
  schedule.fixed_address =
      plan.block_count == 0 ? 0 : (opts.fixed_address % static_cast<std::uint64_t>(plan.block_count));
  schedule.start = plan.block_count == 0 ? 0 : (opts.start % static_cast<std::uint64_t>(plan.block_count));
  schedule.step = opts.step == 0 ? 1 : opts.step;
  if (schedule.kind == schedule_kind::affine && opts.kind == schedule_kind::automatic) {
    schedule.step = choose_coprime_step(plan.block_count, schedule.step);
  }
  schedule.unique_period = resolve_schedule_unique_period(schedule);
  return schedule;
}

[[nodiscard]] inline std::size_t required_online_only_unique_period(const run_plan& plan) noexcept {
  switch (plan.profile) {
  case run_profile::experiment:
  case run_profile::validate:
    return plan.requested_window_size != 0 ? plan.requested_window_size : plan.access_count;
  case run_profile::benchmark_latency:
    return plan.access_count;
  case run_profile::benchmark_throughput:
    return plan.access_count;
  }
  return plan.access_count;
}

inline void validate_base_plan(const run_plan& plan) {
  sn::util::log::ensure(plan.block_count > 0, "resolve_run_plan: block_count must be positive");
  sn::util::log::ensure(plan.leaf_count > 0, "resolve_run_plan: leaf_count must be positive");
  sn::util::log::ensure(plan.access_count > 0, "resolve_run_plan: access_count must be positive");
}

inline void validate_window_plan(const run_plan& plan) {
  if (plan.mode != run_mode::disjoint_windowed) {
    return;
  }

  sn::util::log::ensure(
      plan.requested_window_size > 0, "resolve_run_plan: disjoint_windowed requires a positive window_size"
  );
  sn::util::log::ensure(
      static_cast<std::uint64_t>(plan.requested_window_size) <= plan.schedule.unique_period,
      "resolve_run_plan: window_size exceeds schedule unique period"
  );
}

inline void validate_online_only_plan(const run_plan& plan) {
  if (plan.mode != run_mode::disjoint_online_only) {
    return;
  }

  sn::util::log::ensure(
      static_cast<std::uint64_t>(required_online_only_unique_period(plan)) <= plan.schedule.unique_period,
      "resolve_run_plan: online-only live window exceeds schedule unique period"
  );
}

inline void validate_schedule_plan(const run_plan& plan) {
  if (plan.requires_unique_addresses && plan.schedule.kind == schedule_kind::per_worker_fixed) {
    sn::util::log::fail("resolve_run_plan: per_worker_fixed schedule is invalid for disjoint runs");
  }

  if (plan.requires_unique_addresses && plan.schedule.kind == schedule_kind::fixed &&
      (plan.mode == run_mode::disjoint_windowed || plan.profile == run_profile::experiment)) {
    sn::util::log::fail("resolve_run_plan: fixed schedule is invalid for multi-access disjoint runs");
  }

  if (plan.schedule.kind == schedule_kind::affine && std::gcd(plan.block_count, plan.schedule.step) != 1) {
    sn::util::log::ensure(
        !plan.requires_unique_addresses,
        "resolve_run_plan: affine step must be coprime to block_count for disjoint runs"
    );
  }
}

inline void validate_stateful_parallel_plan(const run_plan& plan) {
  if (!plan.commit_leaf_updates || plan.profile == run_profile::benchmark_latency ||
      plan.profile == run_profile::validate || plan.concurrency <= 1 ||
      plan.schedule.kind == schedule_kind::per_worker_fixed) {
    return;
  }

  sn::util::log::ensure(
      static_cast<std::uint64_t>(plan.access_count) <= plan.schedule.unique_period,
      "resolve_run_plan: stateful parallel runs require the schedule unique period to cover access_count"
  );
}

template <typename Client>
[[nodiscard]] inline run_plan resolve_run_plan(
    const Client& client, const run_options& opts, run_profile profile, std::size_t available_workers
) {
  run_plan plan = build_base_plan(client, opts, profile, available_workers);
  validate_base_plan(plan);
  sn::util::log::ensure(client_supports_mode<Client>(opts.mode), "resolve_run_plan: run mode unsupported by client");

  plan.schedule = resolve_schedule_plan(plan, opts.schedule);
  validate_window_plan(plan);
  validate_online_only_plan(plan);
  validate_schedule_plan(plan);
  validate_stateful_parallel_plan(plan);
  return plan;
}

} // namespace sn::oram::harness::detail
