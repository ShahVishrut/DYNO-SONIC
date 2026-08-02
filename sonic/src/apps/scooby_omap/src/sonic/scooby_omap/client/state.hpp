#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/common/threadpool_support.hpp"
#include "sonic/scooby_node/types/context.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/runtime/epoch.hpp"
#include "sonic/scooby_omap/runtime/host_buffer_pool.hpp"
#include "sonic/scooby_omap/runtime/message.hpp"
#include "sonic/scooby_omap/runtime/metrics.hpp"
#include "sonic/scooby_omap/runtime/pipeline.hpp"
#include "sonic/scooby_omap/runtime/queue.hpp"
#include "sonic/scooby_omap/secure/helpers.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::client {

template <std::size_t PayloadBytes> struct state {
  using request_slot_t = request_slot<key_type, PayloadBytes>;
  using response_slot_t = response_slot<key_type, PayloadBytes>;

  plan_config plan{};
  types::execution_context* ctx{nullptr};
  topology_plan topology{};
  host_buffer_pool tx_pool{};
  std::vector<send_pipeline<client_metrics>> lb_pipelines{};
  std::vector<int> lb_ranks{};
  blocking_queue<inbound_message> inbox{};
  client_metrics metrics{};
  epoch_counts epoch_expectations{};
  epoch_table epochs{epoch_counts{}};
  std::atomic<bool> stop_requested{false};
  sn::sgxbridge::tp::background_task receiver{};
  sn::sgxbridge::time::steady_clock::time_point start_time{sn::sgxbridge::time::steady_clock::now()};
  sn::util::bench::sample_accumulator epoch_stats{sn::util::bench::stat_accumulator_options{
      0,
      true,
      6.0,
      true,
      0,
      0
  }};
  std::vector<request_slot_t> request_slots{};
  std::vector<response_slot_t> response_slots{};
  sn::crypto::buffered_prng<> request_prng{};
  secure_session_t tx_session{};
  secure_session_t rx_session{};
};

inline epoch_counts make_epoch_expectations(const plan_config& plan) {
  epoch_counts counts{};
  counts.client_batches_out = plan.load_balancer_count;
  counts.client_responses_in = plan.load_balancer_count;
  return counts;
}

template <std::size_t PayloadBytes> inline void configure(state<PayloadBytes>& s) {
  host_buffer_pool_config pool_cfg;
  pool_cfg.slot_bytes = s.plan.layout.batch_message_bytes;
  pool_cfg.pool_size = s.plan.client_pipeline_depth * std::max<std::uint32_t>(s.plan.load_balancer_count, 1u);
  s.tx_pool.configure(pool_cfg);
  s.inbox.configure("client", "responses");
  s.epoch_expectations = make_epoch_expectations(s.plan);
  s.epochs = epoch_table(s.epoch_expectations);
  sn::util::log::ensuref(
      s.plan.layout.requests_per_client_batch <= std::numeric_limits<std::size_t>::max(),
      "scooby-omap client batch size exceeds platform limits"
  );
  const std::size_t slots_per_batch = static_cast<std::size_t>(s.plan.layout.requests_per_client_batch);
  s.request_slots.assign(slots_per_batch, typename state<PayloadBytes>::request_slot_t{});
  s.response_slots.assign(slots_per_batch, typename state<PayloadBytes>::response_slot_t{});
  s.request_prng = sn::crypto::buffered_prng<>{};
  const std::size_t payload_bytes = static_cast<std::size_t>(s.plan.layout.batch_payload_bytes);
  secure::configure_client_sessions(s.plan, payload_bytes, s.tx_session, s.rx_session);
}

}
