#pragma once

#include "sonic/scooby_omap/suboram/receiver.hpp"
#include "sonic/scooby_omap/protocol/suboram.hpp"

namespace sn::scooby::omap::suboram {

template <std::size_t PayloadBytes>
inline void encode_bin_response(
    state<PayloadBytes>& s, const bin_dispatch_header& request_header, std::uint64_t epoch,
    sn::util::span<const typename state<PayloadBytes>::bin_slot_t> slots
) {
  send_pipeline<suboram_metrics>& pipeline = s.lb_pipelines[request_header.lb_id];
  pipeline_slot& slot = pipeline.next_slot();
  auto writable = map_writable(*slot.buffer);
  std::size_t wire_bytes{0};
  {
    phase_scope build_scope(s.metrics.phase.response_build);
    wire_bytes = protocol::suboram::build_bin_response<PayloadBytes>(
        s.response_tx_session, s.plan, epoch, s.plan.role_index, request_header, slots, writable
    );
  }
  {
    phase_scope send_scope(s.metrics.phase.response_send);
    pipeline.submit(slot, s.lb_ranks[request_header.lb_id], wire_bytes);
  }
  ++s.metrics.bin_responses_out;
  s.metrics.bin_response_bytes_out += wire_bytes;
  ++s.metrics.messages_sent;
  s.metrics.bytes_sent += wire_bytes;
}

template <std::size_t PayloadBytes> inline bool process_epoch(state<PayloadBytes>& s, std::uint64_t epoch) {
  const std::uint32_t expected = s.plan.load_balancer_count * s.plan.client_count;
  std::uint32_t processed = 0;
  update_pending_peak(s.metrics);

  while (processed < expected) {
    inbound_message msg;
    const auto wait_start = sn::sgxbridge::time::steady_clock::now();
    sn::util::log::ensuref(s.inbox.pop(msg), "scooby-omap suboram inbox closed unexpectedly");
    record_recv_wait(s.metrics.dispatch_recv_wait, s.metrics.dispatch_recv_wait_events, wait_start);
    record_queue_wait(s.metrics, msg.enqueued_at);
    global_header envelope{};
    bin_dispatch_header request_header{};
    bool decoded_ok{false};
    {
      phase_scope decode_scope(s.metrics.phase.bin_decode);
      decoded_ok = protocol::suboram::decode_bin_dispatch<PayloadBytes>(
          s.bin_rx_session, s.plan, epoch, msg.bytes, envelope, request_header, s.bin_slots
      );
    }
    if (!decoded_ok) {
      s.ctx->logger.wrn("scooby-omap suboram failed to decode bin dispatch");
      continue;
    }
    ++s.metrics.bin_dispatch_in;
    s.metrics.bin_dispatch_bytes_in += msg.bytes.size();
    s.epochs.mark(epoch, epoch_event::bin_dispatch_in);
    {
      phase_scope process_scope(s.metrics.phase.backend_process);
      s.backend->process_bin(
          sn::util::span<typename state<PayloadBytes>::bin_slot_t>(s.bin_slots.data(), s.bin_slots.size())
      );
    }
    encode_bin_response(
        s, request_header, epoch,
        sn::util::span<const typename state<PayloadBytes>::bin_slot_t>(s.bin_slots.data(), s.bin_slots.size())
    );
    ++s.metrics.bins_processed;
    s.epochs.mark(epoch, epoch_event::bin_response_out);
    {
      phase_scope maintenance_scope(s.metrics.phase.maintenance);
      s.backend->perform_maintenance();
    }
    ++processed;
  }

  update_pending_peak(s.metrics);
  return true;
}

}
