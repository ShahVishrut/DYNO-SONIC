#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numeric>
#include <string>
#include <vector>

#include "sonic/scooby_node/types/context.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/util/bench/benchstats.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/picoformat.hpp"

namespace sn::scooby::omap {

struct duration_stats {
  sn::util::bench::sample_accumulator acc;

  duration_stats() :
      acc(sn::util::bench::stat_accumulator_options{
          0,
          true,
          6.0,
          true,
          0,
          0
      }) {}

  void add(std::uint64_t ns) { acc.add(static_cast<double>(ns)); }

  std::uint64_t recorded() const { return static_cast<std::uint64_t>(acc.recorded()); }
};

using wait_stats = duration_stats;

struct phase_scope {
  duration_stats* stats{nullptr};
  sn::sgxbridge::time::steady_clock::time_point start{};

  explicit phase_scope(duration_stats& stat) : stats(&stat), start(sn::sgxbridge::time::steady_clock::now()) {}

  ~phase_scope() {
    if (stats == nullptr) {
      return;
    }
    const auto elapsed = sn::sgxbridge::time::since(start);
    stats->add(static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(elapsed)));
  }
};

struct bucket_view {
  sn::util::bench::stats_snapshot snap;
  double total_ns{0.0};
};

inline sn::util::bench::stats_view_options scooby_stats_view() {
  sn::util::bench::stats_view_options view{};
  view.drop_fraction = 0.10;
  view.ci = sn::util::bench::ci_method::student_t;
  return view;
}

inline bucket_view make_bucket_view(const duration_stats& stats, const sn::util::bench::stats_view_options& view) {
  auto snap = stats.acc.snapshot(view);
  bucket_view out{};
  if (!snap.samples.empty()) {
    out.total_ns = std::accumulate(snap.samples.begin(), snap.samples.end(), 0.0);
  } else {
    out.total_ns = snap.mean * static_cast<double>(snap.samples_recorded);
  }
  out.snap = std::move(snap);
  return out;
}

inline bucket_view sum_stats(std::initializer_list<const bucket_view*> parts) {
  bucket_view out{};
  std::vector<double> all_samples;
  double weighted_total = 0.0;
  std::size_t total_count = 0;

  for (const auto* p : parts) {
    if (p == nullptr) {
      continue;
    }
    out.total_ns += p->total_ns;
    out.snap.samples_rejected += p->snap.samples_rejected;
    out.snap.warmup_dropped += p->snap.warmup_dropped;
    if (!p->snap.samples.empty()) {
      all_samples.insert(all_samples.end(), p->snap.samples.begin(), p->snap.samples.end());
    } else if (p->snap.samples_recorded > 0) {
      weighted_total += p->snap.mean * static_cast<double>(p->snap.samples_recorded);
      total_count += p->snap.samples_recorded;
    }
  }

  if (!all_samples.empty()) {
    out.snap.samples = std::move(all_samples);
    out.snap.samples_recorded = out.snap.samples.size();
    out.total_ns = std::accumulate(out.snap.samples.begin(), out.snap.samples.end(), 0.0);
    out.snap.mean = out.total_ns / static_cast<double>(out.snap.samples_recorded);
    std::sort(out.snap.samples.begin(), out.snap.samples.end());
    out.snap.min = out.snap.samples.front();
    out.snap.max = out.snap.samples.back();
    out.snap.median = sn::util::bench::quantile(out.snap.samples, 0.5);
    out.snap.q1 = sn::util::bench::quantile(out.snap.samples, 0.25);
    out.snap.q3 = sn::util::bench::quantile(out.snap.samples, 0.75);
    out.snap.p95 = sn::util::bench::quantile(out.snap.samples, 0.95);
    out.snap.p99 = sn::util::bench::quantile(out.snap.samples, 0.99);
    double variance = 0.0;
    for (double v : out.snap.samples) {
      const double diff = v - out.snap.mean;
      variance += diff * diff;
    }
    if (out.snap.samples_recorded > 1) {
      variance /= static_cast<double>(out.snap.samples_recorded - 1);
      out.snap.stddev = std::sqrt(std::max(0.0, variance));
      out.snap.std_error = out.snap.stddev / std::sqrt(static_cast<double>(out.snap.samples_recorded));
    }
    if (out.snap.mean > 0.0 && out.snap.stddev > 0.0) {
      out.snap.cv_percent = (out.snap.stddev / out.snap.mean) * 100.0;
    }
    out.snap.ci = sn::util::bench::student_t_interval(out.snap.mean, out.snap.stddev, out.snap.samples_recorded);
    return out;
  }

  out.snap.samples_recorded = total_count;
  if (total_count > 0) {
    out.snap.mean = weighted_total / static_cast<double>(total_count);
  }
  return out;
}

inline double pct_of_runtime(const bucket_view& view, std::uint64_t total_runtime_ns) {
  if (total_runtime_ns == 0) {
    return 0.0;
  }
  return (view.total_ns / static_cast<double>(total_runtime_ns)) * 100.0;
}

inline std::string format_bucket(const char* name, const bucket_view& view, std::uint64_t total_runtime_ns) {
  const double avg_ms = view.snap.mean / 1'000'000.0;
  const double p95_ms = view.snap.p95 / 1'000'000.0;
  const double max_ms = view.snap.max / 1'000'000.0;
  return pfm::format(
      "%s=%.3f/%.3f/%.3fms(%.1f%%)", name, avg_ms, p95_ms, max_ms, pct_of_runtime(view, total_runtime_ns)
  );
}

template <typename Metrics> inline void update_pending_peak(Metrics& metrics) {
  const auto pending = sn::sgxbridge::dist::pending_messages();
  if (pending > metrics.pending_messages_peak) {
    metrics.pending_messages_peak = pending;
  }
}

template <typename Metrics>
inline void record_queue_wait(Metrics& metrics, const sn::sgxbridge::time::steady_clock::time_point& enqueued_at) {
  if (enqueued_at == sn::sgxbridge::time::steady_clock::time_point{}) {
    return;
  }
  const auto wait = sn::sgxbridge::time::since(enqueued_at);
  metrics.queue_wait.add(static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(wait)));
}

inline void record_recv_wait(
    wait_stats& stats, std::uint64_t& events, const sn::sgxbridge::time::steady_clock::time_point& wait_started_at
) {
  const auto wait = sn::sgxbridge::time::since(wait_started_at);
  stats.add(static_cast<std::uint64_t>(sn::sgxbridge::time::to_nanoseconds(wait)));
  ++events;
}

inline double bytes_to_gbps(std::uint64_t bytes, std::uint64_t total_runtime_ns) {
  if (total_runtime_ns == 0) {
    return 0.0;
  }
  const double seconds = static_cast<double>(total_runtime_ns) / 1'000'000'000.0;
  return ((static_cast<double>(bytes) * 8.0) / seconds) / 1'000'000'000.0;
}

struct lb_phase_metrics {
  duration_stats client_decode{};
  duration_stats prf_select{};
  duration_stats router_ingest{};
  duration_stats router_route{};
  duration_stats bin_stage{};
  duration_stats bin_build{};
  duration_stats bin_send{};
  duration_stats bin_resp_decode{};
  duration_stats assemble{};
  duration_stats client_resp_build{};
  duration_stats client_resp_send{};
};

struct client_phase_metrics {
  duration_stats request_generate{};
  duration_stats request_build{};
  duration_stats lb_send{};
  duration_stats resp_decode{};
};

struct suboram_phase_metrics {
  duration_stats bin_decode{};
  duration_stats backend_process{};
  duration_stats maintenance{};
  duration_stats response_build{};
  duration_stats response_send{};
};

struct lb_metrics {
  std::uint64_t epochs_started{0};
  std::uint64_t epochs_completed{0};

  std::uint64_t client_batches_in{0};
  std::uint64_t client_batch_bytes_in{0};

  std::uint64_t bin_dispatch_out{0};
  std::uint64_t bin_dispatch_bytes_out{0};

  std::uint64_t bin_responses_in{0};
  std::uint64_t bin_response_bytes_in{0};

  std::uint64_t client_responses_out{0};
  std::uint64_t client_response_bytes_out{0};

  std::uint64_t messages_sent{0};
  std::uint64_t messages_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};

  wait_stats queue_wait{};
  wait_stats pipeline_wait{};
  std::uint64_t pipeline_wait_events{0};
  wait_stats client_recv_wait{};
  std::uint64_t client_recv_wait_events{0};
  wait_stats bin_recv_wait{};
  std::uint64_t bin_recv_wait_events{0};
  std::uint64_t pending_messages_peak{0};
  std::size_t max_queue_depth{0};
  std::uint64_t total_runtime_ns{0};

  lb_phase_metrics phase{};
};

struct client_metrics {
  std::uint64_t epochs_started{0};
  std::uint64_t epochs_completed{0};

  std::uint64_t batch_requests_out{0};
  std::uint64_t batch_request_bytes_out{0};

  std::uint64_t batch_responses_in{0};
  std::uint64_t batch_response_bytes_in{0};

  std::uint64_t messages_sent{0};
  std::uint64_t messages_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};

  wait_stats queue_wait{};
  wait_stats pipeline_wait{};
  std::uint64_t pipeline_wait_events{0};
  wait_stats recv_wait{};
  std::uint64_t recv_wait_events{0};
  std::uint64_t pending_messages_peak{0};
  std::size_t max_queue_depth{0};
  std::uint64_t total_runtime_ns{0};

  client_phase_metrics phase{};
};

struct suboram_metrics {
  std::uint64_t epochs_started{0};
  std::uint64_t epochs_completed{0};

  std::uint64_t bin_dispatch_in{0};
  std::uint64_t bin_dispatch_bytes_in{0};

  std::uint64_t bin_responses_out{0};
  std::uint64_t bin_response_bytes_out{0};

  std::uint64_t bins_processed{0};

  std::uint64_t messages_sent{0};
  std::uint64_t messages_received{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};

  wait_stats queue_wait{};
  wait_stats pipeline_wait{};
  std::uint64_t pipeline_wait_events{0};
  wait_stats dispatch_recv_wait{};
  std::uint64_t dispatch_recv_wait_events{0};
  std::uint64_t pending_messages_peak{0};
  std::size_t max_queue_depth{0};
  std::uint64_t total_runtime_ns{0};

  suboram_phase_metrics phase{};
};

inline void log_load_balancer_metrics(const lb_metrics& m, types::execution_context& ctx) {
  const auto bytes_sent_h = sn::util::humanize::bytes(m.bytes_sent);
  const auto bytes_recv_h = sn::util::humanize::bytes(m.bytes_received);

  const double runtime_s = m.total_runtime_ns > 0 ? static_cast<double>(m.total_runtime_ns) / 1'000'000'000.0 : 0.0;
  const std::string log = pfm::format(
      "scooby lb epochs=%llu/%llu runtime=%.3fs batches=%llu "
      "msgs=%llu/%llu bytes=%s/%s",
      static_cast<unsigned long long>(m.epochs_started), static_cast<unsigned long long>(m.epochs_completed), runtime_s,
      static_cast<unsigned long long>(m.client_batches_in), static_cast<unsigned long long>(m.messages_sent),
      static_cast<unsigned long long>(m.messages_received), bytes_sent_h, bytes_recv_h
  );

  ctx.logger.inf(log);
}

inline void log_client_metrics(const client_metrics& m, types::execution_context& ctx) {
  const auto bytes_sent_h = sn::util::humanize::bytes(m.bytes_sent);
  const auto bytes_recv_h = sn::util::humanize::bytes(m.bytes_received);

  const double runtime_s = m.total_runtime_ns > 0 ? static_cast<double>(m.total_runtime_ns) / 1'000'000'000.0 : 0.0;
  const std::string log = pfm::format(
      "scooby client epochs=%llu/%llu runtime=%.3fs batches=%llu/%llu "
      "msgs=%llu/%llu bytes=%s/%s",
      static_cast<unsigned long long>(m.epochs_started), static_cast<unsigned long long>(m.epochs_completed), runtime_s,
      static_cast<unsigned long long>(m.batch_requests_out), static_cast<unsigned long long>(m.batch_responses_in),
      static_cast<unsigned long long>(m.messages_sent), static_cast<unsigned long long>(m.messages_received),
      bytes_sent_h, bytes_recv_h
  );

  ctx.logger.inf(log);
}

inline void log_suboram_metrics(const suboram_metrics& m, types::execution_context& ctx) {
  const auto bytes_sent_h = sn::util::humanize::bytes(m.bytes_sent);
  const auto bytes_recv_h = sn::util::humanize::bytes(m.bytes_received);

  const double runtime_s = m.total_runtime_ns > 0 ? static_cast<double>(m.total_runtime_ns) / 1'000'000'000.0 : 0.0;
  const std::string log = pfm::format(
      "scooby suboram epochs=%llu/%llu runtime=%.3fs bins=%llu/%llu "
      "msgs=%llu/%llu bytes=%s/%s",
      static_cast<unsigned long long>(m.epochs_started), static_cast<unsigned long long>(m.epochs_completed), runtime_s,
      static_cast<unsigned long long>(m.bin_dispatch_in), static_cast<unsigned long long>(m.bin_responses_out),
      static_cast<unsigned long long>(m.messages_sent), static_cast<unsigned long long>(m.messages_received),
      bytes_sent_h, bytes_recv_h
  );

  ctx.logger.inf(log);
}

}
