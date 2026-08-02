#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "sonic/omap/harness/o2th/config.hpp"
#include "sonic/oram/harness/config.hpp"
#include "sonic/oram/harness/detail/random.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::omap::harness::o2th {

namespace detail {

template <typename Table> using key_type_t = typename Table::key_type;
template <typename Table> using op_request_t = typename Table::op_request;
template <typename Table> using maybe_request_t = typename Table::template maybe_dummy<op_request_t<Table>>;
template <typename Table> using block_data_t = typename Table::data_buffer;
template <typename Table> using data_query_t = typename Table::data_query;
template <typename Table> using bucket_index_t = typename Table::bucket_index;

inline std::size_t clamp_real_request_count(std::size_t requested, std::size_t block_count) {
  if (requested == 0) {
    return block_count;
  }
  return std::min(requested, block_count);
}

inline std::size_t compute_target_writes(std::size_t real_count, double ratio) {
  if (real_count == 0) {
    return 0;
  }
  const double clamped = std::clamp(ratio, 0.0, 1.0);
  const double target = clamped * static_cast<double>(real_count);
  const auto rounded = static_cast<std::size_t>(std::llround(target));
  return std::min(rounded, real_count);
}

inline std::vector<std::size_t> shuffled_indices(std::size_t count, sn::crypto::buffered_prng<>& prng) {
  std::vector<std::size_t> indices(count);
  std::iota(indices.begin(), indices.end(), static_cast<std::size_t>(0));
  if (!indices.empty()) {
    for (std::size_t i = indices.size() - 1; i > 0; --i) {
      const std::size_t j = static_cast<std::size_t>(prng.random_u64(0, static_cast<std::uint64_t>(i)));
      std::swap(indices[i], indices[j]);
    }
  }
  return indices;
}

} // namespace detail

inline std::string format_experiment_summary(const experiment_result& result) {
  const auto build_phase = result.build_phase();
  const auto query_phase = result.query_phase();
  const auto retrieve_phase = result.retrieve_phase();
  const double total_seconds = result.total_seconds();
  const std::size_t total_ops = build_phase.operations + query_phase.operations + retrieve_phase.operations;
  const double total_throughput = total_seconds > 0.0 ? static_cast<double>(total_ops) / total_seconds : 0.0;

  return pfm::format(
      "o2th::experiment summary:\n"
      "  shape: iters=%zu blocks=%zu real=%zu dataset=%zu writes=%zu\n"
      "  total_time: build=%.6f query=%.6f retrieve=%.6f total=%.6f\n"
      "  throughput: build=%.3f query=%.3f retrieve=%.3f total=%.3f\n"
      "  latency/batch: build=%.6f query=%.6f retrieve=%.6f total=%.6f",
      result.iterations, result.block_count, result.real_request_count, result.dataset_queries, result.target_writes,
      build_phase.seconds, query_phase.seconds, retrieve_phase.seconds, total_seconds,
      build_phase.throughput_ops_per_sec(), query_phase.throughput_ops_per_sec(),
      retrieve_phase.throughput_ops_per_sec(), total_throughput, result.build_latency(), result.query_latency(),
      result.retrieve_latency(), result.total_latency()
  );
}

template <typename Table, typename Clock = sn::oram::harness::default_clock_traits>
experiment_result experiment(Table& table, const experiment_options& opts) {
  using key_type = detail::key_type_t<Table>;
  using maybe_request = detail::maybe_request_t<Table>;
  using block_data = detail::block_data_t<Table>;
  using data_query = detail::data_query_t<Table>;
  using bucket_index = detail::bucket_index_t<Table>;

  const std::size_t block_count = table.block_count();
  sn::util::log::ensure(block_count > 0, "o2th::experiment: block_count must be positive");

  const std::size_t iterations = std::max<std::size_t>(opts.iterations, static_cast<std::size_t>(1));
  const std::size_t real_count = detail::clamp_real_request_count(opts.workload.real_request_count, block_count);
  sn::util::log::ensure(real_count > 0, "o2th::experiment: workload requires at least one real request");
  const std::size_t dataset_queries = opts.workload.dataset_queries != 0 ? opts.workload.dataset_queries : block_count;
  const std::size_t target_writes = detail::compute_target_writes(real_count, opts.workload.write_ratio);

  experiment_result result{};
  auto log = sn::util::log::create("o2th:experiment");
  log.vrbf(
      "o2th::experiment: iterations=%zu block_count=%zu real=%zu dataset=%zu writes=%zu", iterations, block_count,
      real_count, dataset_queries, target_writes
  );
  result.iterations = iterations;
  result.block_count = block_count;
  result.real_request_count = real_count;
  result.dataset_queries = dataset_queries;
  result.target_writes = target_writes;
  result.iterations_detail.clear();
  result.iterations_detail.reserve(iterations);

  const std::size_t retrieve_count = block_count * 2;

  std::vector<maybe_request> build_set(block_count);
  std::vector<block_data> dataset(block_count);
  std::vector<maybe_request> retrieved(retrieve_count);
  std::vector<std::uint8_t> marks(retrieve_count);
  std::vector<std::size_t> prefix(retrieve_count + 1);

  std::vector<data_query> batch_queries(dataset_queries);
  std::vector<bucket_index> batch_pos_l1(dataset_queries);
  std::vector<bucket_index> batch_pos_l2(dataset_queries);

  sn::oram::harness::detail::seed_generator seed_gen;

  for (std::size_t iter = 0; iter < iterations; ++iter) {
    experiment_result::iteration_breakdown iter_detail{};
    auto prng = sn::oram::harness::detail::make_prng(seed_gen);

    for (std::size_t idx = 0; idx < block_count; ++idx) {
      auto& req = build_set[idx];
      req.is_dummy = true;
      dataset[idx].fill(0);
    }

    std::vector<std::size_t> real_positions = detail::shuffled_indices(block_count, prng);

    if (real_positions.size() > real_count) {
      real_positions.resize(real_count);
    }

    std::vector<std::size_t> write_positions = real_positions;
    if (write_positions.size() > target_writes) {
      write_positions.resize(target_writes);
    }

    std::vector<bool> is_write(block_count);
    for (std::size_t pos : write_positions) {
      is_write[pos] = true;
    }

    std::uint64_t next_key = 1;

    for (std::size_t pos : real_positions) {
      auto& req = build_set[pos];
      req.is_dummy = false;

      const key_type key = static_cast<key_type>(next_key++);
      req.value.key = key;
      auto& slot = dataset[pos];
      sn::oram::harness::detail::fill_random_buffer(slot, prng);
      const bool write = is_write[pos];
      req.value.is_write = write;
      req.value.data = slot;
    }

    const auto build_single_start = Clock::now();
    table.build(sn::util::span<maybe_request>(build_set.data(), build_set.size()));
    const double build_single_seconds = Clock::seconds_between(build_single_start, Clock::now());
    iter_detail.build_single.seconds = build_single_seconds;
    iter_detail.build_single.operations = block_count;
    result.build_single.seconds += build_single_seconds;
    result.build_single.operations += block_count;

    sn::obliv::fill(marks.begin(), marks.end(), static_cast<std::uint8_t>(0));
    sn::obliv::fill(prefix.begin(), prefix.end(), static_cast<std::size_t>(0));
    for (auto& entry : retrieved) {
      entry.is_dummy = true;
      entry.value = {};
    }

    const auto retrieve_start = Clock::now();
    table.retrieve(
        sn::util::span<maybe_request>(retrieved.data(), retrieved.size()),
        sn::util::span<std::uint8_t>(marks.data(), marks.size()),
        sn::util::span<std::size_t>(prefix.data(), prefix.size())
    );
    const double retrieve_seconds = Clock::seconds_between(retrieve_start, Clock::now());
    iter_detail.retrieve_single.seconds = retrieve_seconds;
    iter_detail.retrieve_single.operations = retrieve_count;
    result.retrieve_single.seconds += retrieve_seconds;
    result.retrieve_single.operations += retrieve_count;

    const auto rebuild_start = Clock::now();
    table.build(sn::util::span<maybe_request>(build_set.data(), build_set.size()));
    const double build_batch_seconds = Clock::seconds_between(rebuild_start, Clock::now());
    iter_detail.build_batch.seconds = build_batch_seconds;
    iter_detail.build_batch.operations = block_count;
    result.build_batch.seconds += build_batch_seconds;
    result.build_batch.operations += block_count;

    std::size_t q_index = 0;
    for (std::size_t pos : real_positions) {
      if (q_index >= dataset_queries) {
        break;
      }
      const auto payload = sn::util::span<const std::uint8_t>(dataset[pos].data(), dataset[pos].size());
      auto& query = batch_queries[q_index];
      query.assign(build_set[pos].value.key, payload, is_write[pos]);
      ++q_index;
    }

    while (q_index < dataset_queries) {
      const key_type dummy_key = static_cast<key_type>(next_key++);
      auto& query = batch_queries[q_index];
      auto payload = sn::util::span<const std::uint8_t>(dataset[0].data(), dataset[0].size());
      query.assign(dummy_key, payload, false);
      ++q_index;
    }

    const auto batch_start = Clock::now();
    table.access_batch(
        sn::util::span<data_query>(batch_queries.data(), batch_queries.size()),
        sn::util::span<bucket_index>(batch_pos_l1.data(), batch_pos_l1.size()),
        sn::util::span<bucket_index>(batch_pos_l2.data(), batch_pos_l2.size())
    );
    const double access_batch_seconds = Clock::seconds_between(batch_start, Clock::now());
    iter_detail.access_batch.seconds = access_batch_seconds;
    iter_detail.access_batch.operations = batch_queries.size();
    result.access_batch.seconds += access_batch_seconds;
    result.access_batch.operations += batch_queries.size();

    sn::obliv::fill(marks.begin(), marks.end(), static_cast<std::uint8_t>(0));
    sn::obliv::fill(prefix.begin(), prefix.end(), static_cast<std::size_t>(0));
    for (auto& entry : retrieved) {
      entry.is_dummy = true;
      entry.value = {};
    }

    const auto retrieve_batch_start = Clock::now();
    table.retrieve(
        sn::util::span<maybe_request>(retrieved.data(), retrieved.size()),
        sn::util::span<std::uint8_t>(marks.data(), marks.size()),
        sn::util::span<std::size_t>(prefix.data(), prefix.size())
    );
    const double retrieve_batch_seconds = Clock::seconds_between(retrieve_batch_start, Clock::now());
    iter_detail.retrieve_batch.seconds = retrieve_batch_seconds;
    iter_detail.retrieve_batch.operations = retrieve_count;
    result.retrieve_batch.seconds += retrieve_batch_seconds;
    result.retrieve_batch.operations += retrieve_count;

    const double iter_build = iter_detail.build_single.seconds + iter_detail.build_batch.seconds;
    const double iter_query = iter_detail.access_batch.seconds;
    const double iter_retrieve = iter_detail.retrieve_single.seconds + iter_detail.retrieve_batch.seconds;
    const double iter_total = iter_build + iter_query + iter_retrieve;
    log.vrbf(
        "o2th::experiment[%zu/%zu]: build=%.6f query=%.6f retrieve=%.6f total=%.6f", iter + 1, iterations, iter_build,
        iter_query, iter_retrieve, iter_total
    );
    result.iterations_detail.push_back(iter_detail);
  }
  log.inf(format_experiment_summary(result));
  log.vrbf("o2th::experiment: finished iterations=%zu total_time=%.6f", result.iterations, result.total_seconds());

  return result;
}

} // namespace sn::omap::harness::o2th
