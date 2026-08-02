#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "sonic/omap/harness/scooby/runner.hpp"

namespace sn::omap::harness::scooby {

template <std::size_t PayloadBytes, typename SubOramConfig> struct validate_plan {
  system_config<PayloadBytes, SubOramConfig> system{};
  std::size_t batches = 1;
  double dummy_ratio = 0.0;
  double write_ratio = 0.0;
  sn::threads::thread_context threads{};
  sn::util::log::logger logger = sn::util::log::create("scooby:validate");
};

template <std::size_t PayloadBytes, typename SubOramConfig>
void validate(const validate_plan<PayloadBytes, SubOramConfig>& plan) {
  using traits = detail::payload_traits<PayloadBytes>;
  using router_type = typename traits::router_type;
  using routed_slot = typename router_type::routed_slot;
  using payload_buffer = typename traits::payload_buffer;

  auto runtime_handle = make_runtime<PayloadBytes, SubOramConfig>(plan.system, plan.threads);
  auto& runtime = *runtime_handle;
  auto log = plan.logger;
  sn::crypto::prng prng;

  std::vector<maybe_request<PayloadBytes>> batch(plan.system.router_cfg.batch_size);
  std::vector<std::vector<routed_slot>> bin_snapshots(runtime.suborams.size());
  std::vector<std::vector<payload_buffer>> shadow_state(runtime.suborams.size());
  for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
    const std::size_t block_count = runtime.suborams[sub]->block_count();
    shadow_state[sub].assign(block_count, payload_buffer{});
  }

  const auto derived = sn::omap::lbrouter::compute_derived_config<sn::omap::harness::scooby::key_type, PayloadBytes>(
      plan.system.router_cfg
  );
  const std::size_t router_bin_capacity = derived.bin_capacity;
  const std::size_t suboram_batch = runtime.suborams.empty() ? 0 : runtime.suborams.front()->batch_size();
  log.inff(
      "scooby::validate: batches=%zu batch_size=%zu suborams=%zu bin_capacity=%zu suboram_batch=%zu dummy=%.3f "
      "write=%.3f",
      plan.batches, plan.system.router_cfg.batch_size, plan.system.suboram_count, router_bin_capacity, suboram_batch,
      plan.dummy_ratio, plan.write_ratio
  );

  for (std::size_t iter = 0; iter < plan.batches; ++iter) {
    const std::size_t batch_index = iter + 1;
    log.vrbf("scooby::validate[%zu/%zu]: prepare", batch_index, plan.batches);
    detail::prepare_batch(runtime, prng, plan.dummy_ratio, plan.write_ratio, batch);

    log.vrbf("scooby::validate[%zu/%zu]: route", batch_index, plan.batches);
    runtime.router.ingest_batch(sn::util::span<const maybe_request<PayloadBytes>>(batch.data(), batch.size()));
    runtime.router.route();

    log.vrbf("scooby::validate[%zu/%zu]: snapshot", batch_index, plan.batches);
    for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
      auto bin_view = runtime.router.bin_view(sub);
      auto& snapshot = bin_snapshots[sub];
      snapshot.assign(bin_view.begin(), bin_view.end());
    }

    log.vrbf("scooby::validate[%zu/%zu]: execute", batch_index, plan.batches);
    for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
      auto bin_view = runtime.router.bin_view(sub);
      runtime.suborams[sub]->process_bin(bin_view);
      runtime.suborams[sub]->maintenance();
    }

    log.vrbf("scooby::validate[%zu/%zu]: verify", batch_index, plan.batches);
    for (std::size_t sub = 0; sub < runtime.suborams.size(); ++sub) {
      auto bin_view = runtime.router.bin_view(sub);
      const auto& snapshot = bin_snapshots[sub];
      sn::util::log::ensure(snapshot.size() == bin_view.size(), "scooby: bin snapshot size mismatch");
      sn::util::span<const routed_slot> input_span(snapshot.data(), snapshot.size());
      sn::util::span<const routed_slot> output_span(bin_view.data(), bin_view.size());
      sn::util::span<payload_buffer> shadow_span(shadow_state[sub].data(), shadow_state[sub].size());
      detail::verify_responses<PayloadBytes>(sub, input_span, output_span, shadow_span);
    }
    log.inff("scooby::validate[%zu/%zu]: ok", batch_index, plan.batches);
  }
}

template <std::size_t PayloadBytes>
using pmchain_validate_plan = validate_plan<PayloadBytes, sn::omap::suboram::pmchain::config>;
template <std::size_t PayloadBytes>
using o2th_validate_plan = validate_plan<PayloadBytes, sn::omap::suboram::o2th::config>;

} // namespace sn::omap::harness::scooby
