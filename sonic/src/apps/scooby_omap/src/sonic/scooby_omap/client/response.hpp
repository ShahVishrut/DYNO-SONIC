#pragma once

#include "sonic/scooby_omap/client/state.hpp"
#include "sonic/scooby_omap/protocol/client.hpp"

namespace sn::scooby::omap::client {

template <std::size_t PayloadBytes> inline bool wait_for_responses(state<PayloadBytes>& s, std::uint64_t epoch) {
  const std::uint32_t expected = s.plan.load_balancer_count;
  std::uint32_t received = 0;
  auto phase_start = sn::sgxbridge::time::steady_clock::now();
  update_pending_peak(s.metrics);
  s.ctx->logger.trcf(
      "scooby-omap client[%u] epoch=%llu phase=recv start expected=%u", s.plan.role_index,
      static_cast<unsigned long long>(epoch), expected
  );
  while (received < expected) {
    inbound_message msg;
    const auto wait_start = sn::sgxbridge::time::steady_clock::now();
    sn::util::log::ensuref(s.inbox.pop(msg), "scooby-omap client inbox closed prematurely");
    record_recv_wait(s.metrics.recv_wait, s.metrics.recv_wait_events, wait_start);
    record_queue_wait(s.metrics, msg.enqueued_at);
    {
      phase_scope decode_scope(s.metrics.phase.resp_decode);
      global_header envelope{};
      batch_response_header header{};
      const bool decoded = protocol::client::decode_batch_response<PayloadBytes>(
          s.rx_session, epoch, s.plan.role_index, msg.bytes, envelope, header, s.response_slots
      );
      sn::util::log::ensuref(decoded, "scooby response");
      for (const auto& slot : s.response_slots) {
        sn::util::log::ensuref(
            slot.source_index < s.plan.layout.requests_per_client_batch, "scooby response"
        );
      }
      ++s.metrics.batch_responses_in;
      s.metrics.batch_response_bytes_in += msg.bytes.size();
      s.epochs.mark(epoch, epoch_event::client_response_in);
    }
    ++received;
  }
  update_pending_peak(s.metrics);
  (void)phase_start;
  return true;
}

}
