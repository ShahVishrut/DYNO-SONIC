#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "sonic/sgxbridge/common/threadpool_support.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/scooby_node/types/context.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/log.hpp"

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

#include "sonic/scooby_omap/suboram/backend.hpp"
#include "sonic/scooby_omap/suboram/factory.hpp"

namespace sn::scooby::omap::suboram {

template <std::size_t PayloadBytes> struct state {
  using bin_slot_t = routed_slot<key_type, PayloadBytes>;

  plan_config plan{};
  types::execution_context* ctx{nullptr};
  topology_plan topology{};
  host_buffer_pool response_pool{};
  std::vector<send_pipeline<suboram_metrics>> lb_pipelines{};
  std::vector<int> lb_ranks{};
  blocking_queue<inbound_message> inbox{};
  suboram_metrics metrics{};
  epoch_counts epoch_expectations{};
  epoch_table epochs{epoch_counts{}};
  std::atomic<bool> stop_requested{false};
  sn::sgxbridge::tp::background_task receiver{};
  sn::sgxbridge::time::steady_clock::time_point start_time{sn::sgxbridge::time::steady_clock::now()};
  sn::sgxbridge::tp::session o2th_pool{};
  sn::sgxbridge::tp::session pmchain_pool{};
  std::unique_ptr<suboram_backend<PayloadBytes>> backend{};
  std::vector<bin_slot_t> bin_slots{};
  secure_session_t bin_rx_session{};
  secure_session_t response_tx_session{};
};

inline epoch_counts make_epoch_expectations(const plan_config& plan) {
  epoch_counts counts{};
  const std::uint32_t expected_bins = plan.load_balancer_count * plan.client_count;
  counts.bin_dispatches_in = expected_bins;
  counts.bin_responses_out = expected_bins;
  return counts;
}

template <std::size_t PayloadBytes> inline bool configure(state<PayloadBytes>& s) {
  host_buffer_pool_config pool_cfg;
  pool_cfg.slot_bytes = s.plan.layout.bin_message_bytes;
  pool_cfg.pool_size = std::max<std::uint32_t>(1u, s.plan.load_balancer_count * s.plan.suboram_pipeline_depth);
  s.response_pool.configure(pool_cfg);
  s.inbox.configure("suboram", "bins");
  s.epoch_expectations = make_epoch_expectations(s.plan);
  s.epochs = epoch_table(s.epoch_expectations);
  if (!initialize_backend(s)) {
    return false;
  }
  s.bin_slots.assign(static_cast<std::size_t>(s.plan.layout.bin_capacity), typename state<PayloadBytes>::bin_slot_t{});
  const std::size_t bin_bytes = static_cast<std::size_t>(s.plan.layout.bin_payload_bytes);
  secure::configure_suboram_sessions(s.plan, bin_bytes, s.bin_rx_session, s.response_tx_session);
  return true;
}

}
