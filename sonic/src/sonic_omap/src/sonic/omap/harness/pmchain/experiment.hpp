#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "sonic/crypto/prng.hpp"
#include "sonic/util/picoformat.hpp"
#include "sonic/oram/harness/config.hpp"
#include "sonic/omap/harness/pmchain/validate.hpp"

namespace sn::omap::harness::pmchain {

struct experiment_options {
  std::size_t batches = 1;
  double write_ratio = 0.5;
  sn::util::log::logger logger = sn::util::log::create("pmchain:experiment");
};

struct stage_breakdown {
  double populate_seconds = 0.0;
  double o2th_seconds = 0.0;
  double sort_seconds = 0.0;
  double oram_seconds = 0.0;
  double flush_seconds = 0.0;

  [[nodiscard]] double process_seconds() const noexcept {
    return populate_seconds + o2th_seconds + sort_seconds + oram_seconds;
  }
  [[nodiscard]] double total_seconds() const noexcept { return process_seconds() + flush_seconds; }
};

struct experiment_result {
  std::size_t batches_executed = 0;
  std::size_t requests_per_batch = 0;
  std::size_t writes_per_batch = 0;
  stage_breakdown totals{};
  std::vector<stage_breakdown> per_batch{};

  [[nodiscard]] std::size_t total_requests() const noexcept { return batches_executed * requests_per_batch; }
  [[nodiscard]] double populate_seconds() const noexcept { return totals.populate_seconds; }
  [[nodiscard]] double o2th_seconds() const noexcept { return totals.o2th_seconds; }
  [[nodiscard]] double sort_seconds() const noexcept { return totals.sort_seconds; }
  [[nodiscard]] double oram_seconds() const noexcept { return totals.oram_seconds; }
  [[nodiscard]] double flush_seconds() const noexcept { return totals.flush_seconds; }
  [[nodiscard]] double chain_seconds() const noexcept { return populate_seconds() + o2th_seconds() + sort_seconds(); }
  [[nodiscard]] double process_seconds() const noexcept { return totals.process_seconds(); }
  [[nodiscard]] double online_seconds() const noexcept { return chain_seconds() + oram_seconds(); }
  [[nodiscard]] double total_seconds() const noexcept { return online_seconds() + flush_seconds(); }

  [[nodiscard]] double throughput_chain() const noexcept { return throughput(total_requests(), chain_seconds()); }
  [[nodiscard]] double throughput_oram() const noexcept { return throughput(total_requests(), oram_seconds()); }
  [[nodiscard]] double throughput_online() const noexcept { return throughput(total_requests(), online_seconds()); }

  [[nodiscard]] double latency_chain() const noexcept { return average(chain_seconds()); }
  [[nodiscard]] double latency_oram() const noexcept { return average(oram_seconds()); }
  [[nodiscard]] double latency_offline() const noexcept { return average(flush_seconds()); }
  [[nodiscard]] double latency_online() const noexcept { return average(online_seconds()); }
  [[nodiscard]] double latency_total() const noexcept { return average(total_seconds()); }

private:
  [[nodiscard]] double average(double seconds) const noexcept {
    return batches_executed > 0 ? seconds / static_cast<double>(batches_executed) : 0.0;
  }

  static double throughput(std::size_t ops, double seconds) noexcept {
    return seconds > 0.0 ? static_cast<double>(ops) / seconds : 0.0;
  }
};

inline std::string format_experiment_summary(const experiment_result& result) {
  return pfm::format(
      "pmchain::experiment summary:\n"
      "  shape: batches=%zu batch=%zu writes=%zu total=%zu\n"
      "  total_time: populate=%.6f o2th=%.6f sort=%.6f oram=%.6f flush=%.6f chain=%.6f online=%.6f total=%.6f\n"
      "  throughput: chain=%.3f oram=%.3f online=%.3f\n"
      "  latency/batch: chain=%.6f oram=%.6f offline=%.6f online=%.6f total=%.6f",
      result.batches_executed, result.requests_per_batch, result.writes_per_batch, result.total_requests(),
      result.populate_seconds(), result.o2th_seconds(), result.sort_seconds(), result.oram_seconds(),
      result.flush_seconds(), result.chain_seconds(), result.online_seconds(), result.total_seconds(),
      result.throughput_chain(), result.throughput_oram(), result.throughput_online(), result.latency_chain(),
      result.latency_oram(), result.latency_offline(), result.latency_online(), result.latency_total()
  );
}

template <typename Chain, typename Clock = sn::oram::harness::default_clock_traits>
experiment_result experiment(Chain& chain, const experiment_options& opts = {}) {
  auto log = opts.logger;
  const std::size_t batch_size = chain.batch_size();
  const std::size_t block_bytes = chain.block_size();
  const std::size_t block_count = chain.block_count();

  sn::util::log::ensure(batch_size > 0, "pmchain::experiment: batch size must be positive");
  sn::util::log::ensure(block_bytes > 0, "pmchain::experiment: block size must be positive");
  sn::util::log::ensure(block_count > 0, "pmchain::experiment: block count must be positive");
  sn::util::log::ensure(opts.batches > 0, "pmchain::experiment: batches must be positive");

  const double clamped_ratio = std::clamp(opts.write_ratio, 0.0, 1.0);
  log.vrbf(
      "pmchain::experiment: batches=%zu batch_size=%zu block_count=%zu block_bytes=%zu write_ratio=%.3f", opts.batches,
      batch_size, block_count, block_bytes, clamped_ratio
  );

  sn::crypto::prng prng;

  detail::batch_buffers<Chain> buffers(batch_size);

  const std::size_t writes_per_batch =
      static_cast<std::size_t>(std::llround(clamped_ratio * static_cast<double>(batch_size)));

  experiment_result result{};
  result.requests_per_batch = batch_size;
  result.writes_per_batch = writes_per_batch;
  result.per_batch.reserve(opts.batches);
  for (std::size_t batch = 0; batch < opts.batches; ++batch) {
    detail::prepare_batch(chain, buffers, prng, batch, writes_per_batch);

    const auto populate_start = Clock::now();
    chain.populate_requests(sn::util::span<const typename Chain::operation>(buffers.ops.data(), buffers.ops.size()));
    const auto populate_end = Clock::now();
    chain.execute_o2th_chains();
    const auto o2th_end = Clock::now();
    chain.sort_o2th_chains();
    const auto sort_end = Clock::now();
    chain.execute_oram_queries();
    const auto oram_end = Clock::now();
    chain.flush_pending();
    const auto flush_end = Clock::now();

    stage_breakdown timings{};
    timings.populate_seconds = Clock::seconds_between(populate_start, populate_end);
    timings.o2th_seconds = Clock::seconds_between(populate_end, o2th_end);
    timings.sort_seconds = Clock::seconds_between(o2th_end, sort_end);
    timings.oram_seconds = Clock::seconds_between(sort_end, oram_end);
    timings.flush_seconds = Clock::seconds_between(oram_end, flush_end);

    result.totals.populate_seconds += timings.populate_seconds;
    result.totals.o2th_seconds += timings.o2th_seconds;
    result.totals.sort_seconds += timings.sort_seconds;
    result.totals.oram_seconds += timings.oram_seconds;
    result.totals.flush_seconds += timings.flush_seconds;
    result.per_batch.push_back(timings);
    ++result.batches_executed;

    log.vrbf(
        "pmchain::experiment[%zu/%zu]: writes=%zu populate=%.6fs o2th=%.6fs sort=%.6fs oram=%.6fs flush=%.6fs "
        "total=%.6fs",
        batch + 1, opts.batches, writes_per_batch, timings.populate_seconds, timings.o2th_seconds, timings.sort_seconds,
        timings.oram_seconds, timings.flush_seconds, timings.total_seconds()
    );
  }

  log.inf(format_experiment_summary(result));
  return result;
}

} // namespace sn::omap::harness::pmchain
