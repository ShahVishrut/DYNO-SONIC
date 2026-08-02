#pragma once

#include <numeric>
#include <vector>

#include "sonic/sgxbridge/dist/api.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/runtime/epoch_driver.hpp"
#include "sonic/scooby_omap/wire/payload.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"

#include "sonic/scooby_omap/client/receiver.hpp"
#include "sonic/scooby_omap/client/request.hpp"
#include "sonic/scooby_omap/client/response.hpp"
#include "sonic/scooby_omap/client/state.hpp"

namespace sn::scooby::omap {

namespace client {

inline void log_epoch_stats(const sn::util::bench::sample_accumulator& acc, types::execution_context& ctx) {
  auto view = scooby_stats_view();
  const auto snap = acc.snapshot(view);
  const std::size_t seen = snap.samples_recorded + snap.samples_rejected + snap.warmup_dropped;
  if (seen == 0) {
    return;
  }
  auto to_ms = [](double ns) { return ns / 1'000'000.0; };
  ctx.logger.inf(
      pfm::format(
          "scooby epoch count=%zu/%zu mean=%.3f median=%.3f p95=%.3f max=%.3f drop=%zu reject=%zu",
          snap.samples_recorded, seen, to_ms(snap.mean), to_ms(snap.median), to_ms(snap.p95), to_ms(snap.max),
          snap.warmup_dropped, snap.samples_rejected
      )
  );
}

template <std::size_t PayloadBytes>
types::command_result run_impl(const plan_config& plan, types::execution_context& ctx) {
  client::state<PayloadBytes> s;
  s.plan = plan;
  s.ctx = &ctx;
  s.topology = build_topology(plan);
  client::configure(s);
  s.lb_ranks.resize(plan.load_balancer_count);
  s.lb_pipelines.resize(plan.load_balancer_count);
  for (std::uint32_t lb = 0; lb < plan.load_balancer_count; ++lb) {
    s.lb_ranks[lb] = load_balancer_rank(s.topology, lb);
    buffer_tag tag{endpoint_label(endpoint_kind::client), "lb_request", lb};
    s.lb_pipelines[lb].initialize(s.tx_pool, plan.client_pipeline_depth, tag);
    s.lb_pipelines[lb].attach_metrics(&s.metrics);
  }

  if (!client::start_receiver(s)) {
    client::stop_receiver(s);
    return types::make_result(types::result_status::internal_error, "client failed to start receiver task");
  }
  auto per_epoch = [&](std::uint64_t epoch) {
    const auto epoch_start = sn::sgxbridge::time::steady_clock::now();
    if (!client::send_batches_for_epoch(s, epoch)) {
      return false;
    }
    if (!client::wait_for_responses(s, epoch)) {
      return false;
    }
    const double epoch_duration =
        static_cast<double>(sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(epoch_start)));
    s.epoch_stats.add(epoch_duration);
    if (plan.telemetry_interval_epochs > 0 && (epoch + 1) % plan.telemetry_interval_epochs == 0) {
      s.metrics.total_runtime_ns =
          static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(s.start_time)));
      s.metrics.max_queue_depth = std::max(s.metrics.max_queue_depth, s.inbox.max_depth());
      log_client_metrics(s.metrics, ctx);
    }
    return true;
  };
  auto after_epoch = [&](std::uint64_t epoch) {
    auto summary = s.epochs.describe(epoch);
    if (!summary.empty()) {
      s.ctx->logger.vrbf(
          "scooby-omap client[%u] epoch=%llu complete %s", s.plan.role_index, static_cast<unsigned long long>(epoch),
          summary
      );
      if (!s.epochs.complete(epoch)) {
        sn::util::log::ensuref(
            false, "scooby-omap client[%u] epoch=%llu incomplete %s", s.plan.role_index,
            static_cast<unsigned long long>(epoch), summary
        );
      }
    }
  };

  const bool ok = drive_epochs(s, plan, endpoint_kind::client, per_epoch, after_epoch);

  for (auto& pipeline : s.lb_pipelines) {
    pipeline.drain();
  }
  client::stop_receiver(s);
  sn::sgxbridge::dist::barrier();

  s.metrics.total_runtime_ns =
      static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(sn::sgxbridge::time::since(s.start_time)));
  s.metrics.max_queue_depth = std::max(s.metrics.max_queue_depth, s.inbox.max_depth());
  log_client_metrics(s.metrics, ctx);

  log_epoch_stats(s.epoch_stats, ctx);

  if (!ok) {
    return types::make_result(types::result_status::internal_error, "client encountered error");
  }
  return types::make_result(types::result_status::ok);
}

}

inline types::command_result run_client(const plan_config& plan, types::execution_context& ctx) {
  return dispatch_payload(plan.payload_bytes, [&](auto tag) { return client::run_impl<tag.value>(plan, ctx); });
}

}
