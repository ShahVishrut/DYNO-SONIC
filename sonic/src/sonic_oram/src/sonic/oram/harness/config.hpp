#pragma once

#include <cstddef>
#include <cstdint>
#include "sonic/threads/thread_pool.hpp"
#include "sonic/util/bench/minibench.hpp"
#include "sonic/util/harness_clock.hpp"

namespace sn::oram::harness {

enum class run_mode {
  // normal eviction scheduling
  standard,
  // disjoint eviction after windows of accesses
  disjoint_windowed,
  // online phase only, excluding eviction
  disjoint_online_only,
};

// access schedule type; automatic will choose the cheapest one
enum class schedule_kind {
  automatic,
  fixed,
  per_worker_fixed,
  round_robin,
  affine,
};

enum class schedule_scope {
  // advance across the whole run
  stream,
  // restart pattern for each window
  window,
};

struct worker_pool_config {
  sn::threads::thread_pool* pool = nullptr;
  std::size_t max_workers = 0; // 0 => auto
};

struct schedule_options {
  schedule_kind kind = schedule_kind::automatic;
  schedule_scope scope = schedule_scope::stream;
  std::uint64_t fixed_address = 0;
  std::uint64_t start = 0;
  std::uint64_t step = 1;
};

struct run_options {
  std::size_t access_count = 0; // 0 => block_count
  run_mode mode = run_mode::standard;
  std::size_t window_size = 0; // 0 => auto
  std::uint64_t seed = 0;      // 0 => random
  schedule_options schedule{};
  worker_pool_config workers{};
};

struct validate_options {
  run_options run{};
  std::size_t iterations = 1;
  std::size_t batch_accesses = 0;
};

struct validate_result {
  std::size_t dummy_probe_accesses = 0;
  std::size_t round_trip_accesses = 0;
  std::size_t batch_accesses = 0;
};

struct experiment_options {
  run_options run{};
};

struct experiment_result {
  std::size_t access_count = 0;
  std::size_t concurrency = 1;
  double elapsed_seconds = 0.0;
  double throughput_ops_per_sec = 0.0;
  double throughput_bytes_per_sec = 0.0;
};

struct benchmark_options {
  run_options run{};
  sn::util::bench::latency_options latency{};
  sn::util::bench::throughput_options throughput{};
};

struct benchmark_result {
  sn::util::bench::latency_result latency{};
  sn::util::bench::throughput_result throughput{};
};

using default_clock_traits = sn::util::clock::monotonic;

[[nodiscard]] inline constexpr bool requires_disjoint_window(run_mode mode) noexcept {
  return mode != run_mode::standard;
}

} // namespace sn::oram::harness
