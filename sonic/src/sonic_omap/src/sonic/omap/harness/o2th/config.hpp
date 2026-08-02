#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sn::omap::harness::o2th {

struct workload_shape {
  std::size_t real_request_count = 0;
  std::size_t dataset_queries = 0;
  double write_ratio = 0.5;
};

struct phase_metrics {
  double seconds = 0.0;
  std::size_t operations = 0;

  [[nodiscard]] double throughput_ops_per_sec() const noexcept {
    return seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
  }

  [[nodiscard]] bool has_work() const noexcept { return operations > 0 && seconds > 0.0; }
};

struct experiment_options {
  std::size_t iterations = 1;
  workload_shape workload{};
};

struct validate_options {
  std::size_t iterations = 1;
};

struct validate_result {
  std::size_t access_one_accesses = 0;
  std::size_t dummy_probe_accesses = 0;
  std::size_t batch_accesses = 0;
};

struct experiment_result {
  std::size_t iterations = 0;
  std::size_t block_count = 0;
  std::size_t real_request_count = 0;
  std::size_t dataset_queries = 0;
  std::size_t target_writes = 0;
  phase_metrics build_single{};
  phase_metrics retrieve_single{};
  phase_metrics build_batch{};
  phase_metrics access_batch{};
  phase_metrics retrieve_batch{};

  [[nodiscard]] phase_metrics build_phase() const noexcept { return combine(build_single, build_batch); }
  [[nodiscard]] phase_metrics query_phase() const noexcept { return access_batch; }
  [[nodiscard]] phase_metrics retrieve_phase() const noexcept { return combine(retrieve_single, retrieve_batch); }

  [[nodiscard]] double total_seconds() const noexcept {
    return build_single.seconds + retrieve_single.seconds + build_batch.seconds + access_batch.seconds +
           retrieve_batch.seconds;
  }

  [[nodiscard]] double build_seconds() const noexcept { return build_phase().seconds; }
  [[nodiscard]] double query_seconds() const noexcept { return query_phase().seconds; }
  [[nodiscard]] double retrieve_seconds() const noexcept { return retrieve_phase().seconds; }

  [[nodiscard]] double build_latency() const noexcept { return average(build_seconds()); }
  [[nodiscard]] double query_latency() const noexcept { return average(query_seconds()); }
  [[nodiscard]] double retrieve_latency() const noexcept { return average(retrieve_seconds()); }
  [[nodiscard]] double total_latency() const noexcept { return average(total_seconds()); }

  struct iteration_breakdown {
    phase_metrics build_single{};
    phase_metrics retrieve_single{};
    phase_metrics build_batch{};
    phase_metrics access_batch{};
    phase_metrics retrieve_batch{};
  };

  std::vector<iteration_breakdown> iterations_detail{};

private:
  [[nodiscard]] static phase_metrics combine(const phase_metrics& lhs, const phase_metrics& rhs) noexcept {
    phase_metrics out{};
    out.seconds = lhs.seconds + rhs.seconds;
    out.operations = lhs.operations + rhs.operations;
    return out;
  }

  [[nodiscard]] double average(double seconds) const noexcept {
    return iterations > 0 ? seconds / static_cast<double>(iterations) : 0.0;
  }
};

struct benchmark_options {
  std::size_t iterations = 1;
  workload_shape workload{};
};

struct benchmark_result {
  phase_metrics build{};
  phase_metrics access_batch{};
  phase_metrics retrieve{};
};

} // namespace sn::omap::harness::o2th
