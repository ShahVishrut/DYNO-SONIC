#pragma once

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/runtime/epoch_driver.hpp"
#include "sonic/scooby_omap/wire/payload.hpp"

#include "sonic/scooby_omap/suboram/process.hpp"

namespace sn::scooby::omap {

namespace suboram {

template <std::size_t PayloadBytes>
types::command_result run_impl(const plan_config& plan, types::execution_context& ctx) {
  suboram::state<PayloadBytes> s;
  s.plan = plan;
  s.ctx = &ctx;
  s.topology = build_topology(plan);
  if (!suboram::configure(s)) {
    return types::make_result(types::result_status::internal_error, "suboram configuration failed");
  }
  s.lb_ranks.resize(plan.load_balancer_count);
  s.lb_pipelines.resize(plan.load_balancer_count);
  for (std::uint32_t i = 0; i < plan.load_balancer_count; ++i) {
    s.lb_ranks[i] = load_balancer_rank(s.topology, i);
    buffer_tag tag{endpoint_label(endpoint_kind::suboram), "lb_response", i};
    s.lb_pipelines[i].initialize(s.response_pool, plan.suboram_pipeline_depth, tag);
    s.lb_pipelines[i].attach_metrics(&s.metrics);
  }
  if (!suboram::start_receiver(s)) {
    suboram::stop_receiver(s);
    return types::make_result(types::result_status::internal_error, "suboram failed to start receiver task");
  }

  auto per_epoch = [&](std::uint64_t epoch) { return suboram::process_epoch(s, epoch); };
  auto after_epoch = [&](std::uint64_t epoch) {
    auto summary = s.epochs.describe(epoch);
    if (!summary.empty()) {
      s.ctx->logger.vrbf(
          "scooby-omap suboram[%u] epoch=%llu complete %s", s.plan.role_index, static_cast<unsigned long long>(epoch),
          summary
      );
      sn::util::log::ensuref(
          s.epochs.complete(epoch), "scooby-omap suboram[%u] epoch=%llu incomplete %s", s.plan.role_index,
          static_cast<unsigned long long>(epoch), summary
      );
    }
  };

  const bool ok = drive_epochs(s, plan, endpoint_kind::suboram, per_epoch, after_epoch);

  for (auto& pipeline : s.lb_pipelines) {
    pipeline.drain();
  }
  if (s.backend) {
    s.backend->shutdown();
  }
  suboram::stop_receiver(s);
  sn::sgxbridge::dist::barrier();
  s.metrics.total_runtime_ns =
      static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(s.start_time)));
  s.metrics.max_queue_depth = std::max(s.metrics.max_queue_depth, s.inbox.max_depth());
  log_suboram_metrics(s.metrics, ctx);

  if (!ok) {
    return types::make_result(types::result_status::internal_error, "suboram encountered error");
  }
  return types::make_result(types::result_status::ok);
}

}

inline types::command_result run_suboram(const plan_config& plan, types::execution_context& ctx) {
  return dispatch_payload(plan.payload_bytes, [&](auto tag) { return suboram::run_impl<tag.value>(plan, ctx); });
}

}
