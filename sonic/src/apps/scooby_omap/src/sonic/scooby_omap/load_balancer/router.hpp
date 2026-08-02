#pragma once

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/scooby_omap/load_balancer/state.hpp"
#include "sonic/scooby_omap/load_balancer/wire_helpers.hpp"
#include "sonic/scooby_omap/protocol/load_balancer.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> inline bool acquire_router(state<PayloadBytes>& s) {
  const std::uint32_t logical = std::max<std::uint32_t>(s.plan.lbrouter_parallelism, std::uint32_t{1});
  const std::uint32_t background = static_cast<std::uint32_t>(sn::threads::background_threads_for_parallelism(logical));
  const auto req = sn::sgxbridge::tp::make_request(
      background, static_cast<std::uint32_t>(s.plan.router.batch_size),
      sn::sgxbridge::tp::queue_policy::block_when_full, "scooby-omap.lbrouter"
  );
  if (!sn::sgxbridge::tp::acquire_session(s.router_session, s.ctx->threadpools, req, s.ctx->logger)) {
    return false;
  }

  typename router_types<PayloadBytes>::config cfg{};
  cfg.batch_size = s.plan.router.batch_size;
  cfg.suboram_count = s.plan.router.suboram_count;
  cfg.security_parameter_lambda = s.plan.router.lambda;
  cfg.invalid_key = invalid_key;

  try {
    s.router = std::make_unique<sn::omap::lbrouter::lbrouter<key_type, PayloadBytes>>(
        cfg, sn::threads::thread_team(*s.router_session.pool(), logical)
    );
    s.router->initialize();
    s.selector = std::make_unique<routing_selector<PayloadBytes>>(
        s.plan.suboram_count, sn::threads::thread_team(*s.router_session.pool(), logical)
    );
  } catch (const std::exception&) {
    s.ctx->logger.err("scooby router");
    return false;
  }

  s.router_batch.assign(
      static_cast<std::size_t>(s.plan.router.batch_size),
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>{}
  );
  s.router_reassembled.assign(
      static_cast<std::size_t>(s.plan.router.batch_size),
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>{}
  );
  return true;
}

template <std::size_t PayloadBytes>
inline void prepare_router_batch(
    state<PayloadBytes>& s, const typename state<PayloadBytes>::request_slot_t* slots, std::size_t count
) {
  s.routed_batch.resize(count);
  {
    phase_scope prf_scope(s.metrics.phase.prf_select);
    s.selector->assign(
        sn::util::span<const typename state<PayloadBytes>::request_slot_t>(slots, count),
        sn::util::span<typename state<PayloadBytes>::routed_slot_t>(s.routed_batch.data(), s.routed_batch.size())
    );
  }
  for (std::size_t ix = 0; ix < count; ++ix) {
    const auto& wire = s.routed_batch[ix];
    auto& entry = s.router_batch[ix];
    entry.is_dummy = wire.is_dummy();
    entry.value.key = wire.key;
    entry.value.suboram_index = wire.suboram_index;
    entry.value.is_write = wire.is_write();
    sn::obliv::copy_n(wire.payload.begin(), PayloadBytes, entry.value.payload.begin());
  }
}

template <std::size_t PayloadBytes>
inline void send_router_bin(
    state<PayloadBytes>& s, std::uint64_t epoch, std::uint32_t client_id, std::uint32_t suboram_id,
    sn::util::span<const typename router_types<PayloadBytes>::routed_slot> bin_view
) {
  using routed_slot_t = typename state<PayloadBytes>::routed_slot_t;
  sn::util::log::ensuref(
      bin_view.size() == s.plan.layout.bin_capacity, "scooby-omap lb[%u] router bin size mismatch client=%u suboram=%u",
      s.plan.role_index, client_id, suboram_id
  );
  send_pipeline<lb_metrics>& pipeline = s.suboram_pipelines[suboram_id];
  pipeline_slot& slot = pipeline.next_slot();
  auto writable = map_writable(*slot.buffer);
  std::size_t wire_bytes{0};
  {
    phase_scope build_scope(s.metrics.phase.bin_build);
    wire_bytes = protocol::load_balancer::build_bin_dispatch<PayloadBytes>(
        s.bin_tx_session, s.plan, epoch, s.plan.role_index, suboram_id, client_id, writable,
        [&](sn::util::span<std::uint8_t> payload) {
          phase_scope stage_scope(s.metrics.phase.bin_stage);
          const auto slot_bytes = slot_size<routed_slot_t>();
          sn::util::log::ensuref(
              payload.size() == slot_bytes * s.plan.layout.bin_capacity,
              "scooby-omap lb[%u] bin payload size mismatch payload=%zu slots=%zu", s.plan.role_index, payload.size(),
              s.plan.layout.bin_capacity
          );
          std::size_t offset = 0;
          for (std::size_t slot_ix = 0; slot_ix < s.plan.layout.bin_capacity; ++slot_ix, offset += slot_bytes) {
            if (slot_ix < bin_view.size()) {
              routed_slot_t wire = make_wire_slot<PayloadBytes>(suboram_id, bin_view[slot_ix]);
              std::uint8_t* dst = payload.data() + offset;
              sn::obliv::memcpy(dst, &wire, slot_bytes);
            } else {
              routed_slot_t wire{};
              wire.source_index = static_cast<std::uint32_t>(slot_ix);
              wire.key = invalid_key;
              wire.suboram_index = suboram_id;
              wire.flags = 0;
              wire.set_dummy(true);
              wire.set_write(false);
              zero_payload(wire);
              std::uint8_t* dst = payload.data() + offset;
              sn::obliv::memcpy(dst, &wire, slot_bytes);
            }
          }
        }
    );
  }
  {
    phase_scope send_scope(s.metrics.phase.bin_send);
    pipeline.submit(slot, s.suboram_ranks[suboram_id], wire_bytes);
  }
  ++s.metrics.bin_dispatch_out;
  s.metrics.bin_dispatch_bytes_out += wire_bytes;
  ++s.metrics.messages_sent;
  s.metrics.bytes_sent += wire_bytes;
}

}
