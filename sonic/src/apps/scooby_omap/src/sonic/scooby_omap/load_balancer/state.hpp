#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "sonic/crypto/prf.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/omap/lbrouter/client.hpp"
#include "sonic/sgxbridge/common/threadpool_support.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/scooby_node/types/context.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/log.hpp"

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/config/topology.hpp"
#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/load_balancer/epoch_ctx.hpp"
#include "sonic/scooby_omap/load_balancer/types.hpp"
#include "sonic/scooby_omap/load_balancer/routing.hpp"
#include "sonic/scooby_omap/runtime/epoch.hpp"
#include "sonic/scooby_omap/runtime/host_buffer_pool.hpp"
#include "sonic/scooby_omap/runtime/message.hpp"
#include "sonic/scooby_omap/runtime/metrics.hpp"
#include "sonic/scooby_omap/runtime/pipeline.hpp"
#include "sonic/scooby_omap/runtime/queue.hpp"
#include "sonic/scooby_omap/secure/helpers.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> struct state {
  using request_slot_t = request_slot<key_type, PayloadBytes>;
  using routed_slot_t = routed_slot<key_type, PayloadBytes>;
  using response_slot_t = response_slot<key_type, PayloadBytes>;

  plan_config plan{};
  types::execution_context* ctx{nullptr};
  topology_plan topology{};
  host_buffer_pool bin_pool{};
  host_buffer_pool response_pool{};
  std::vector<send_pipeline<lb_metrics>> suboram_pipelines{};
  std::vector<send_pipeline<lb_metrics>> client_pipelines{};
  std::vector<int> suboram_ranks{};
  std::vector<int> client_ranks{};
  blocking_queue<inbound_message> client_inbox{};
  blocking_queue<inbound_message> suboram_inbox{};
  lb_metrics metrics{};
  epoch_counts epoch_expectations{};
  epoch_table epochs{epoch_counts{}};
  std::atomic<bool> stop_requested{false};
  sn::sgxbridge::tp::background_task receiver{};
  sn::sgxbridge::time::steady_clock::time_point start_time{sn::sgxbridge::time::steady_clock::now()};
  sn::sgxbridge::tp::session router_session{};
  std::unique_ptr<sn::omap::lbrouter::lbrouter<key_type, PayloadBytes>> router{};
  std::unique_ptr<routing_selector<PayloadBytes>> selector{};
  std::vector<typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>>
      router_batch{};
  std::vector<typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>>
      router_reassembled{};
  std::vector<response_slot_t> assembled{};
  std::vector<routed_slot_t> routed_batch{};
  epoch_ctx<PayloadBytes> epoch_state{};
  secure_session_t client_rx_session{};
  secure_session_t client_tx_session{};
  secure_session_t bin_rx_session{};
  secure_session_t bin_tx_session{};
};

inline epoch_counts make_epoch_expectations(const plan_config& plan) {
  epoch_counts counts{};
  counts.client_batches_in = plan.client_count;
  const std::uint32_t bin_count = plan.client_count * plan.suboram_count;
  counts.bin_dispatches_out = bin_count;
  counts.bin_responses_in = bin_count;
  counts.client_responses_out = plan.client_count;
  return counts;
}

template <std::size_t PayloadBytes> inline bool configure(state<PayloadBytes>& s) {
  using response_slot_t = typename state<PayloadBytes>::response_slot_t;

  host_buffer_pool_config bin_cfg;
  bin_cfg.slot_bytes = s.plan.layout.bin_message_bytes;
  bin_cfg.pool_size = std::max<std::uint32_t>(1u, s.plan.suboram_count * s.plan.lb_pipeline_depth);
  s.bin_pool.configure(bin_cfg);

  host_buffer_pool_config response_cfg;
  response_cfg.slot_bytes = s.plan.layout.batch_response_bytes;
  response_cfg.pool_size = std::max<std::uint32_t>(1u, s.plan.client_count * s.plan.lb_pipeline_depth);
  s.response_pool.configure(response_cfg);

  s.client_inbox.configure("load_balancer", "client_batches");
  s.suboram_inbox.configure("load_balancer", "bin_responses");

  s.epoch_expectations = make_epoch_expectations(s.plan);
  s.epochs = epoch_table(s.epoch_expectations);
  s.epoch_state = epoch_ctx<PayloadBytes>(s.plan);
  s.assembled.assign(static_cast<std::size_t>(s.plan.layout.requests_per_client_batch), response_slot_t{});
  const std::size_t batch_bytes = static_cast<std::size_t>(s.plan.layout.batch_payload_bytes);
  const std::size_t bin_bytes = static_cast<std::size_t>(s.plan.layout.bin_payload_bytes);
  secure::configure_load_balancer_sessions(
      s.plan, batch_bytes, bin_bytes, s.client_rx_session, s.client_tx_session, s.bin_rx_session, s.bin_tx_session
  );
  return true;
}

}
