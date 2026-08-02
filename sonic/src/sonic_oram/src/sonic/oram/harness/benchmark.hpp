#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/harness/detail/access_stream.hpp"
#include "sonic/oram/harness/detail/access_window.hpp"
#include "sonic/oram/harness/detail/client_access.hpp"
#include "sonic/oram/harness/detail/leaf_table.hpp"
#include "sonic/oram/harness/detail/run_plan.hpp"
#include "sonic/oram/harness/detail/workers.hpp"
#include "sonic/util/bench/minibench.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::harness {

namespace detail {

struct benchmark_session {
  run_plan latency_plan{};
  run_plan throughput_plan{};
  std::optional<sn::threads::thread_team> workers{};
  leaf_table leaves{};
};

[[nodiscard]] inline bool is_online_only(const run_plan& plan) noexcept {
  return plan.mode == run_mode::disjoint_online_only;
}

[[nodiscard]] inline std::size_t latency_window_size(const run_plan& plan) noexcept {
  if (plan.mode == run_mode::standard) {
    return 0;
  }
  if (is_online_only(plan)) {
    return 1;
  }
  if (plan.mode == run_mode::disjoint_windowed) {
    return plan.requested_window_size;
  }
  return std::size_t{1};
}

[[nodiscard]] inline std::size_t throughput_window_size(const run_plan& plan, std::uint64_t batch_iterations) noexcept {
  if (plan.mode == run_mode::standard) {
    return 0;
  }
  if (plan.mode == run_mode::disjoint_windowed) {
    return plan.requested_window_size;
  }
  return static_cast<std::size_t>(batch_iterations != 0 ? batch_iterations : plan.access_count);
}

[[nodiscard]] inline run_options resolve_benchmark_run(const benchmark_options& opts) {
  auto run = opts.run;
  run.seed = resolve_run_seed(run.seed);
  return run;
}

template <typename Client>
[[nodiscard]] inline run_plan make_benchmark_plan(
    Client& client, const run_options& run, run_profile profile, std::size_t available_workers
) {
  return resolve_run_plan(client, run, profile, available_workers);
}

[[nodiscard]] inline leaf_table make_benchmark_leaves(const run_plan& plan) {
  seed_stream seeds(plan.seed);
  return make_random_leaf_table(static_cast<std::size_t>(plan.block_count), plan.leaf_count, seeds);
}

[[nodiscard]] inline std::uint64_t combined_latency_stream_base(const benchmark_session& session) noexcept {
  if (!is_online_only(session.throughput_plan)) {
    return 0;
  }
  return static_cast<std::uint64_t>(session.throughput_plan.access_count);
}

template <typename Client>
[[nodiscard]] benchmark_session make_combined_benchmark_session(Client& client, const benchmark_options& opts) {
  benchmark_session session{};
  auto run = resolve_benchmark_run(opts);

  auto workers = resolve_worker_setup(run.workers);
  session.workers = std::move(workers.parallel);
  session.latency_plan = make_benchmark_plan(client, run, run_profile::benchmark_latency, 1);
  session.throughput_plan = make_benchmark_plan(client, run, run_profile::benchmark_throughput, workers.worker_count);

  session.leaves = make_benchmark_leaves(session.latency_plan);
  return session;
}

template <typename Client, typename WorkerState>
inline void issue_benchmark_access(
    Client& client, WorkerState& state, const run_plan& plan, leaf_table& leaves, const access_coord& coord
) {
  const std::uint64_t address = plan.schedule.address(coord);
  const std::uint64_t cur_leaf = leaves.current(address);
  const std::uint64_t new_leaf = sample_leaf(state.rng, plan.leaf_count);

  issue_access(
      client, state.scratch, static_cast<std::int64_t>(address), cur_leaf, new_leaf, false, state.in_buf, state.out_buf
  );
  if (plan.commit_leaf_updates) {
    leaves.commit(address, new_leaf);
  }
}

template <typename AccessFn>
inline void run_online_only_batch(
    std::optional<sn::threads::thread_team>& workers, std::size_t worker_count, std::uint64_t batch_base,
    std::size_t batch_size, std::uint64_t window_index, AccessFn&& access_one
) {
  (void) run_access_span(
      workers, worker_count, batch_base, batch_size, 0, window_index, std::forward<AccessFn>(access_one),
      [](std::uint64_t) {}
  );
}

template <typename AccessFn, typename CloseFn>
[[nodiscard]] inline std::uint64_t run_windowed_batch(
    std::optional<sn::threads::thread_team>& workers, std::size_t worker_count, std::uint64_t batch_base,
    std::size_t batch_size, std::size_t live_window, std::uint64_t next_window_index, AccessFn&& access_one,
    CloseFn&& close_window
) {
  return run_access_span(
      workers, worker_count, batch_base, batch_size, live_window, next_window_index, std::forward<AccessFn>(access_one),
      std::forward<CloseFn>(close_window)
  );
}

template <typename traits_t, typename Client>
sn::util::bench::throughput_result run_throughput(
    Client& client, const benchmark_options& opts, const run_plan& plan, leaf_table& leaves,
    std::optional<sn::threads::thread_team>& workers, std::uint64_t& next_global_index
) {
  seed_stream seeds(plan.seed);
  const std::size_t worker_count = stream_worker_count(plan.concurrency, plan.access_count, plan.requested_window_size);
  auto states = make_stream_worker_states<traits_t::block_t::byte_size>(client, worker_count, seeds);

  std::uint64_t next_window_index = 0;

  auto run_batch = [&](std::uint64_t batch_iterations, std::uint64_t) {
    if (batch_iterations == 0) {
      return;
    }

    const std::uint64_t batch_base = next_global_index;
    next_global_index += batch_iterations;
    const std::size_t live_window = throughput_window_size(plan, batch_iterations);
    const std::size_t batch_size = static_cast<std::size_t>(batch_iterations);

    auto access_one = [&](std::size_t worker_index, const access_coord& coord) {
      issue_benchmark_access(client, states[worker_index], plan, leaves, coord);
    };

    if (is_online_only(plan)) {
      // online only throughput batch: run batch then drop window
      run_online_only_batch(workers, states.size(), batch_base, batch_size, next_window_index, access_one);
      apply_window_close(client, plan.close);
      ++next_window_index;
      return;
    }

    next_window_index += run_windowed_batch(
        workers, states.size(), batch_base, batch_size, live_window, next_window_index, access_one,
        [&](std::uint64_t) { apply_window_close(client, plan.close); }
    );
  };

  auto throughput_opts = opts.throughput;
  throughput_opts.iteration_count = plan.access_count;
  return sn::util::bench::measure_throughput(throughput_opts.iteration_count, run_batch, throughput_opts);
}

template <typename traits_t, typename Client>
sn::util::bench::latency_result run_latency(
    Client& client, const benchmark_options& opts, const run_plan& plan, leaf_table& leaves,
    std::uint64_t& next_global_index
) {
  seed_stream seeds(plan.seed);
  auto states = make_stream_worker_states<traits_t::block_t::byte_size>(client, 1, seeds);
  auto& state = states.front();
  access_window_tracker window(latency_window_size(plan));

  auto callable = [&](int) {
    const auto token = window.before_access();
    const access_coord coord{next_global_index++, token.window_index, token.slot_in_window, 0};
    issue_benchmark_access(client, state, plan, leaves, coord);

    if (window.after_access(token)) {
      apply_window_close(client, plan.close);
      window.complete_close();
    }
  };

  auto result = sn::util::bench::measure_latency(callable, opts.latency);
  if (window.finalize()) {
    apply_window_close(client, plan.close);
    window.complete_close();
  }
  return result;
}

} // namespace detail

template <typename traits_t, typename Client>
sn::util::bench::latency_result benchmark_latency(Client& client, const benchmark_options& opts) {
  auto run = detail::resolve_benchmark_run(opts);
  (void) detail::resolve_worker_setup(run.workers);

  const detail::run_plan plan = detail::make_benchmark_plan(client, run, detail::run_profile::benchmark_latency, 1);
  auto leaves = detail::make_benchmark_leaves(plan);
  std::uint64_t next_global_index = 0;
  return detail::run_latency<traits_t, Client>(client, opts, plan, leaves, next_global_index);
}

template <typename traits_t, typename Client>
sn::util::bench::throughput_result benchmark_throughput(Client& client, const benchmark_options& opts) {
  auto run = detail::resolve_benchmark_run(opts);

  auto workers = detail::resolve_worker_setup(run.workers);
  const detail::run_plan plan =
      detail::make_benchmark_plan(client, run, detail::run_profile::benchmark_throughput, workers.worker_count);
  auto leaves = detail::make_benchmark_leaves(plan);
  std::uint64_t next_global_index = 0;
  return detail::run_throughput<traits_t, Client>(client, opts, plan, leaves, workers.parallel, next_global_index);
}

template <typename traits_t, typename Client>
benchmark_result benchmark(Client& client, const benchmark_options& opts) {
  auto session = detail::make_combined_benchmark_session(client, opts);
  benchmark_result result{};
  if (detail::is_online_only(session.throughput_plan)) {
    std::uint64_t throughput_global_index = 0;
    result.throughput = detail::run_throughput<traits_t, Client>(
        client, opts, session.throughput_plan, session.leaves, session.workers, throughput_global_index
    );

    std::uint64_t latency_global_index = detail::combined_latency_stream_base(session);
    result.latency =
        detail::run_latency<traits_t, Client>(client, opts, session.latency_plan, session.leaves, latency_global_index);
    return result;
  }

  std::uint64_t latency_global_index = 0;
  std::uint64_t throughput_global_index = 0;
  result.latency =
      detail::run_latency<traits_t, Client>(client, opts, session.latency_plan, session.leaves, latency_global_index);
  result.throughput = detail::run_throughput<traits_t, Client>(
      client, opts, session.throughput_plan, session.leaves, session.workers, throughput_global_index
  );
  return result;
}

} // namespace sn::oram::harness
