#pragma once

#include "sonic/scooby_omap/load_balancer/receiver.hpp"
#include "sonic/scooby_omap/load_balancer/router.hpp"
#include "sonic/scooby_omap/protocol/load_balancer.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes>
inline void stage_router_responses_for_client(state<PayloadBytes>& s, std::uint32_t client, std::uint64_t epoch) {
  for (std::uint32_t suboram = 0; suboram < s.plan.suboram_count; ++suboram) {
    auto& bin_ctx = s.epoch_state.bin_ctx(client, suboram, s.plan);
    sn::util::log::ensuref(
        bin_ctx.seen, "scooby response", s.plan.role_index,
        client, suboram, static_cast<unsigned long long>(epoch)
    );

    auto bin_view = s.router->bin_view(suboram);
    sn::util::log::ensuref(
        bin_ctx.slots.size() == bin_view.size(),
        "scooby response", s.plan.role_index,
        client, suboram, static_cast<std::size_t>(bin_view.size()), static_cast<std::size_t>(bin_ctx.slots.size())
    );

    for (std::size_t ix = 0; ix < bin_view.size(); ++ix) {
      const auto& src = bin_ctx.slots[ix];
      auto& dst = bin_view[ix];
      dst.item.key = static_cast<key_type>(src.key);
      dst.item.suboram_index = suboram;
      dst.item.is_write = src.is_write();
      sn::obliv::copy_n(src.payload.begin(), PayloadBytes, dst.item.payload.begin());
      dst.source_index = src.source_index;
      dst.is_dummy = src.is_dummy();
      dst.io_tag = sn::omap::lbrouter::k_slot_tag_response;
    }
  }
}

template <std::size_t PayloadBytes>
inline void reassemble_for_client(state<PayloadBytes>& s, std::uint32_t client, std::uint64_t epoch) {
  using maybe_dummy_request_t =
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>;

  const std::size_t batch_size = static_cast<std::size_t>(s.plan.router.batch_size);
  auto& client_ctx = s.epoch_state.clients[client];
  sn::util::log::ensuref(
      client_ctx.seen, "scooby-omap lb[%u] missing client batch client=%u epoch=%llu", s.plan.role_index, client,
      static_cast<unsigned long long>(epoch)
  );
  sn::util::log::ensuref(
      client_ctx.slots.size() == batch_size, "scooby-omap lb[%u] batch size mismatch", s.plan.role_index
  );

  prepare_router_batch(s, client_ctx.slots.data(), batch_size);
  s.router->ingest_batch(sn::util::span<const maybe_dummy_request_t>(s.router_batch.data(), batch_size));
  s.router->route();
  stage_router_responses_for_client(s, client, epoch);
  s.router->reassemble(
      sn::util::span<const maybe_dummy_request_t>(s.router_batch.data(), batch_size),
      sn::util::span<maybe_dummy_request_t>(s.router_reassembled.data(), s.router_reassembled.size())
  );

  sn::util::log::ensuref(
      s.assembled.size() == batch_size, "scooby-omap lb[%u] assembled size mismatch", s.plan.role_index
  );
  for (std::size_t ix = 0; ix < batch_size; ++ix) {
    const auto& src = s.router_reassembled[ix];
    auto& dst = s.assembled[ix];
    dst.source_index = static_cast<std::uint32_t>(ix);
    dst.key = static_cast<key_type>(src.value.key);
    dst.flags = 0;
    dst.set_dummy(src.is_dummy);
    dst.set_write(src.value.is_write);
    sn::obliv::copy_n(src.value.payload.begin(), PayloadBytes, dst.payload.begin());
  }
}

template <std::size_t PayloadBytes> inline bool collect_client_batches(state<PayloadBytes>& s, std::uint64_t epoch) {
  s.epoch_state.reset_clients();
  const std::uint32_t expected = s.plan.client_count;
  std::uint32_t seen = 0;
  update_pending_peak(s.metrics);
  while (seen < expected) {
    inbound_message msg;
    const auto wait_start = sn::sgxbridge::time::steady_clock::now();
    sn::util::log::ensuref(s.client_inbox.pop(msg), "scooby-omap lb client inbox closed unexpectedly");
    record_recv_wait(s.metrics.client_recv_wait, s.metrics.client_recv_wait_events, wait_start);
    record_queue_wait(s.metrics, msg.enqueued_at);
    bool decoded_msg{false};
    {
      phase_scope decode_scope(s.metrics.phase.client_decode);
      global_header envelope{};
      batch_request_header header{};
      std::uint32_t client_id = 0;
      sn::util::span<const std::uint8_t> payload{};
      decoded_msg = protocol::load_balancer::decode_batch_request<PayloadBytes>(
          s.client_rx_session, s.plan, epoch, msg.bytes, envelope, header, client_id, payload
      );
      if (decoded_msg) {
        auto& client_ctx = s.epoch_state.clients[client_id];
        sn::util::log::ensuref(
            !client_ctx.seen, "scooby-omap lb[%u] duplicate client batch client=%u epoch=%llu", s.plan.role_index,
            client_id, static_cast<unsigned long long>(epoch)
        );
        decode_slots(payload, client_ctx.slots);
        client_ctx.client_id = client_id;
        client_ctx.seen = true;
      }
    }
    sn::util::log::ensuref(decoded_msg, "scooby-omap lb[%u] failed to decode client batch", s.plan.role_index);
    ++seen;
    ++s.metrics.client_batches_in;
    s.metrics.client_batch_bytes_in += msg.bytes.size();
    s.epochs.mark(epoch, epoch_event::client_batch_in);
  }
  update_pending_peak(s.metrics);
  return true;
}

template <std::size_t PayloadBytes> inline bool dispatch_bins(state<PayloadBytes>& s, std::uint64_t epoch) {
  update_pending_peak(s.metrics);
  const auto batch_size = static_cast<std::size_t>(s.plan.router.batch_size);
  for (std::uint32_t client = 0; client < s.plan.client_count; ++client) {
    auto& ctx = s.epoch_state.clients[client];
    sn::util::log::ensuref(
        ctx.seen, "scooby-omap lb[%u] missing client batch client=%u epoch=%llu", s.plan.role_index, client,
        static_cast<unsigned long long>(epoch)
    );
    sn::util::log::ensuref(ctx.slots.size() == batch_size, "scooby-omap lb[%u] batch size mismatch", s.plan.role_index);
    prepare_router_batch(s, ctx.slots.data(), batch_size);
    {
      phase_scope ingest_scope(s.metrics.phase.router_ingest);
      s.router->ingest_batch(
          sn::util::span<const typename router_types<PayloadBytes>::template maybe_dummy<
              typename router_types<PayloadBytes>::request>>(s.router_batch.data(), batch_size)
      );
    }
    {
      phase_scope route_scope(s.metrics.phase.router_route);
      s.router->route();
    }
    for (std::uint32_t suboram = 0; suboram < s.plan.suboram_count; ++suboram) {
      auto bin_view = s.router->bin_view(suboram);
      send_router_bin(s, epoch, client, suboram, bin_view);
      s.epochs.mark(epoch, epoch_event::bin_dispatch_out);
    }
  }
  update_pending_peak(s.metrics);
  return true;
}

template <std::size_t PayloadBytes> inline bool collect_bin_responses(state<PayloadBytes>& s, std::uint64_t epoch) {
  s.epoch_state.reset_bins();
  const std::uint32_t expected = s.plan.client_count * s.plan.suboram_count;
  std::uint32_t received = 0;
  while (received < expected) {
    inbound_message msg;
    const auto wait_start = sn::sgxbridge::time::steady_clock::now();
    sn::util::log::ensuref(s.suboram_inbox.pop(msg), "scooby-omap lb bin inbox closed unexpectedly");
    record_recv_wait(s.metrics.bin_recv_wait, s.metrics.bin_recv_wait_events, wait_start);
    record_queue_wait(s.metrics, msg.enqueued_at);
    bool decoded{false};
    {
      phase_scope decode_scope(s.metrics.phase.bin_resp_decode);
      global_header envelope{};
      bin_response_header header{};
      std::uint32_t client_id = 0;
      std::uint32_t suboram_id = 0;
      sn::util::span<const std::uint8_t> payload{};
      decoded = protocol::load_balancer::decode_bin_response<PayloadBytes>(
          s.bin_rx_session, s.plan, epoch, msg.bytes, envelope, header, client_id, suboram_id, payload
      );
      if (decoded) {
        auto& bin_ctx = s.epoch_state.bin_ctx(client_id, suboram_id, s.plan);
        sn::util::log::ensuref(
            !bin_ctx.seen, "scooby-omap lb[%u] duplicate bin response client=%u suboram=%u epoch=%llu",
            s.plan.role_index, client_id, suboram_id, static_cast<unsigned long long>(epoch)
        );
        decode_slots(payload, bin_ctx.slots);
        bin_ctx.seen = true;
      }
    }
    sn::util::log::ensuref(decoded, "scooby-omap lb[%u] failed to decode bin response", s.plan.role_index);
    ++received;
    ++s.metrics.bin_responses_in;
    s.metrics.bin_response_bytes_in += msg.bytes.size();
    s.epochs.mark(epoch, epoch_event::bin_response_in);
  }
  update_pending_peak(s.metrics);
  return true;
}

template <std::size_t PayloadBytes> inline bool send_client_responses(state<PayloadBytes>& s, std::uint64_t epoch) {
  using response_slot_t = typename state<PayloadBytes>::response_slot_t;

  update_pending_peak(s.metrics);
  for (std::uint32_t client = 0; client < s.plan.client_count; ++client) {
    auto& assembled = s.assembled;
    {
      phase_scope assemble_scope(s.metrics.phase.assemble);
      reassemble_for_client(s, client, epoch);
    }
    send_pipeline<lb_metrics>& pipeline = s.client_pipelines[client];
    pipeline_slot& slot = pipeline.next_slot();
    auto writable = map_writable(*slot.buffer);
    std::size_t wire_bytes{0};
    {
      phase_scope build_scope(s.metrics.phase.client_resp_build);
      wire_bytes = protocol::load_balancer::build_batch_response<PayloadBytes>(
          s.client_tx_session, s.plan, epoch, s.plan.role_index, client,
          sn::util::span<const response_slot_t>(assembled.data(), assembled.size()), writable
      );
    }
    {
      phase_scope send_scope(s.metrics.phase.client_resp_send);
      pipeline.submit(slot, s.client_ranks[client], wire_bytes);
    }
    ++s.metrics.client_responses_out;
    s.metrics.client_response_bytes_out += wire_bytes;
    ++s.metrics.messages_sent;
    s.metrics.bytes_sent += wire_bytes;
    s.epochs.mark(epoch, epoch_event::client_response_out);
  }
  update_pending_peak(s.metrics);
  return true;
}

}
