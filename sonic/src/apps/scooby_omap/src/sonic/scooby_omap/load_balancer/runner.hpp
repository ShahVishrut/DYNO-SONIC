#pragma once

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/runtime/epoch_driver.hpp"
#include "sonic/scooby_omap/wire/payload.hpp"

#include "sonic/scooby_omap/load_balancer/flow.hpp"

namespace sn::scooby::omap {

namespace load_balancer {

template <std::size_t PayloadBytes>
types::command_result run_impl(const plan_config& plan, types::execution_context& ctx) {
  load_balancer::state<PayloadBytes> s;
  s.plan = plan;
  s.ctx = &ctx;
  s.topology = build_topology(plan);
  if (!load_balancer::configure(s)) {
    return types::make_result(types::result_status::internal_error, "load balancer configuration failed");
  }

  s.suboram_ranks.resize(plan.suboram_count);
  s.suboram_pipelines.resize(plan.suboram_count);
  for (std::uint32_t i = 0; i < plan.suboram_count; ++i) {
    s.suboram_ranks[i] = suboram_rank(s.topology, i);
    buffer_tag tag{endpoint_label(endpoint_kind::load_balancer), "bin", i};
    s.suboram_pipelines[i].initialize(s.bin_pool, plan.lb_pipeline_depth, tag);
    s.suboram_pipelines[i].attach_metrics(&s.metrics);
  }
  s.client_ranks.resize(plan.client_count);
  s.client_pipelines.resize(plan.client_count);
  for (std::uint32_t i = 0; i < plan.client_count; ++i) {
    s.client_ranks[i] = client_rank(s.topology, i);
    buffer_tag tag{endpoint_label(endpoint_kind::load_balancer), "client_resp", i};
    s.client_pipelines[i].initialize(s.response_pool, plan.lb_pipeline_depth, tag);
    s.client_pipelines[i].attach_metrics(&s.metrics);
  }
  if (!load_balancer::acquire_router(s)) {
    return types::make_result(types::result_status::internal_error, "load balancer failed to acquire router threads");
  }
  if (!load_balancer::start_receiver(s)) {
    load_balancer::stop_receiver(s);
    return types::make_result(types::result_status::internal_error, "load balancer failed to start receiver task");
  }

  auto per_epoch = [&](std::uint64_t epoch) {
    return collect_client_batches(s, epoch) && dispatch_bins(s, epoch) && collect_bin_responses(s, epoch) &&
           send_client_responses(s, epoch);
  };
  auto after_epoch = [&](std::uint64_t epoch) {
    auto summary = s.epochs.describe(epoch);
    if (!summary.empty()) {
      s.ctx->logger.vrbf(
          "scooby-omap lb[%u] epoch=%llu complete %s", s.plan.role_index, static_cast<unsigned long long>(epoch),
          summary
      );
      sn::util::log::ensuref(
          s.epochs.complete(epoch), "scooby-omap lb[%u] epoch=%llu incomplete %s", s.plan.role_index,
          static_cast<unsigned long long>(epoch), summary
      );
    }
  };

  const bool ok = drive_epochs(s, plan, endpoint_kind::load_balancer, per_epoch, after_epoch);

  for (auto& pipeline : s.suboram_pipelines) {
    pipeline.drain();
  }
  for (auto& pipeline : s.client_pipelines) {
    pipeline.drain();
  }
  load_balancer::stop_receiver(s);
  sn::sgxbridge::dist::barrier();
  s.metrics.total_runtime_ns =
      static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(s.start_time)));
  s.metrics.max_queue_depth =
      std::max(s.metrics.max_queue_depth, std::max(s.client_inbox.max_depth(), s.suboram_inbox.max_depth()));
  log_load_balancer_metrics(s.metrics, ctx);

  if (!ok) {
    return types::make_result(types::result_status::internal_error, "load balancer encountered error");
  }
  return types::make_result(types::result_status::ok);
}

}

inline types::command_result run_load_balancer(const plan_config& plan, types::execution_context& ctx) {
  return dispatch_payload(plan.payload_bytes, [&](auto tag) { return load_balancer::run_impl<tag.value>(plan, ctx); });
}

}
