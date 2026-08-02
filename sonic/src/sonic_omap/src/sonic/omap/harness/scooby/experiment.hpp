#pragma once

#include <chrono>
#include <vector>

#include "sonic/omap/harness/scooby/runner.hpp"

namespace sn::omap::harness::scooby {

template <std::size_t PayloadBytes, typename SubOramConfig> struct experiment_plan {
  system_config<PayloadBytes, SubOramConfig> system{};
  std::size_t batches = 1;
  double dummy_ratio = 0.0;
  double write_ratio = 0.0;
  sn::threads::thread_context threads{};
  sn::util::log::logger logger = sn::util::log::create("scooby:experiment");
};

struct experiment_result {
  std::size_t batches = 0;
  double route_seconds = 0.0;
  double suboram_seconds = 0.0;
  double total_seconds = 0.0;

  double throughput() const {
    const double total_ops = static_cast<double>(batches);
    return total_seconds > 0.0 ? total_ops / total_seconds : 0.0;
  }
};

template <std::size_t PayloadBytes, typename SubOramConfig>
experiment_result experiment(const experiment_plan<PayloadBytes, SubOramConfig>& plan) {
  auto runtime_handle = make_runtime<PayloadBytes, SubOramConfig>(plan.system, plan.threads);
  auto& runtime = *runtime_handle;
  auto log = plan.logger;
  sn::crypto::prng prng;

  experiment_result result{};
  result.batches = plan.batches;

  std::vector<maybe_request<PayloadBytes>> batch(plan.system.router_cfg.batch_size);
  for (std::size_t iter = 0; iter < plan.batches; ++iter) {
    detail::prepare_batch(runtime, prng, plan.dummy_ratio, plan.write_ratio, batch);

    const auto begin = std::chrono::steady_clock::now();
    runtime.router.ingest_batch(sn::util::span<const maybe_request<PayloadBytes>>(batch.data(), batch.size()));
    runtime.router.route();
    const auto routed = std::chrono::steady_clock::now();

    for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
      auto span = runtime.router.bin_view(sub);
      runtime.suborams[sub]->process_bin(span);
      runtime.suborams[sub]->maintenance();
    }

    const auto end = std::chrono::steady_clock::now();
    result.route_seconds += std::chrono::duration<double>(routed - begin).count();
    result.suboram_seconds += std::chrono::duration<double>(end - routed).count();
  }

  result.total_seconds = result.route_seconds + result.suboram_seconds;
  log.inff(
      "scooby::experiment: batches=%zu route=%.6fs suborams=%.6fs total=%.6fs", result.batches, result.route_seconds,
      result.suboram_seconds, result.total_seconds
  );

  return result;
}

// type alias for suboram types
template <std::size_t PayloadBytes>
using pmchain_experiment_plan = experiment_plan<PayloadBytes, sn::omap::suboram::pmchain::config>;
template <std::size_t PayloadBytes>
using o2th_experiment_plan = experiment_plan<PayloadBytes, sn::omap::suboram::o2th::config>;

} // namespace sn::omap::harness::scooby
