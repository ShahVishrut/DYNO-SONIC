#pragma once

#include "sonic/scooby_omap/client/request_gen.hpp"
#include "sonic/scooby_omap/client/state.hpp"
#include "sonic/scooby_omap/protocol/client.hpp"

namespace sn::scooby::omap::client {

template <std::size_t PayloadBytes> inline void populate_request_slots(state<PayloadBytes>& s) {
  populate_request_slots<PayloadBytes>(
      s.plan.suboram_block_count, s.request_prng,
      sn::util::span<typename state<PayloadBytes>::request_slot_t>(s.request_slots.data(), s.request_slots.size())
  );
}

template <std::size_t PayloadBytes>
inline void submit_batch_request(state<PayloadBytes>& s, std::uint64_t epoch, std::uint32_t lb_index) {
  send_pipeline<client_metrics>& pipeline = s.lb_pipelines[lb_index];
  pipeline_slot& slot = pipeline.next_slot();
  auto writable = map_writable(*slot.buffer);
  std::size_t wire_bytes{0};
  {
    phase_scope gen_scope(s.metrics.phase.request_generate);
    populate_request_slots(s);
  }
  {
    phase_scope build_scope(s.metrics.phase.request_build);
    wire_bytes = protocol::client::build_batch_request<PayloadBytes>(
        s.tx_session, s.plan, epoch, s.plan.role_index, lb_index,
        sn::util::span<const typename state<PayloadBytes>::request_slot_t>(
            s.request_slots.data(), s.request_slots.size()
        ),
        writable
    );
  }
  {
    phase_scope send_scope(s.metrics.phase.lb_send);
    pipeline.submit(slot, s.lb_ranks[lb_index], wire_bytes);
  }
  ++s.metrics.batch_requests_out;
  s.metrics.batch_request_bytes_out += wire_bytes;
  ++s.metrics.messages_sent;
  s.metrics.bytes_sent += wire_bytes;
  s.epochs.mark(epoch, epoch_event::client_batch_out);
}

template <std::size_t PayloadBytes> inline bool send_batches_for_epoch(state<PayloadBytes>& s, std::uint64_t epoch) {
  update_pending_peak(s.metrics);
  for (std::uint32_t lb = 0; lb < s.plan.load_balancer_count; ++lb) {
    submit_batch_request(s, epoch, lb);
  }
  return true;
}

}
