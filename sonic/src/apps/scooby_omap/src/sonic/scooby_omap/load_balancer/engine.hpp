#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "sonic/omap/lbrouter/client.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/load_balancer/epoch_ctx.hpp"
#include "sonic/scooby_omap/load_balancer/routing.hpp"
#include "sonic/scooby_omap/load_balancer/types.hpp"
#include "sonic/scooby_omap/load_balancer/wire_helpers.hpp"
#include "sonic/scooby_omap/protocol/load_balancer.hpp"
#include "sonic/scooby_omap/secure/helpers.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes> struct engine_state {
  using request_slot_t = request_slot<key_type, PayloadBytes>;
  using routed_slot_t = routed_slot<key_type, PayloadBytes>;
  using response_slot_t = response_slot<key_type, PayloadBytes>;
  using lbrouter_t = sn::omap::lbrouter::lbrouter<key_type, PayloadBytes>;

  plan_config plan{};

  std::unique_ptr<lbrouter_t> router{};
  std::unique_ptr<routing_selector<PayloadBytes>> selector{};

  std::vector<typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>>
      router_batch{};
  std::vector<typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>>
      router_reassembled{};
  std::vector<routed_slot_t> routed_batch{};
  std::vector<response_slot_t> assembled{};
  epoch_ctx<PayloadBytes> epoch_state{};

  secure_session_t client_rx_session{};
  secure_session_t client_tx_session{};
  secure_session_t bin_rx_session{};
  secure_session_t bin_tx_session{};

  std::uint64_t routed_epoch{0};
  std::uint32_t routed_client{0};
  bool routed_valid{false};
};

template <std::size_t PayloadBytes>
inline void configure(engine_state<PayloadBytes>& s, const plan_config& plan, sn::threads::thread_team workers) {
  using response_slot_t = typename engine_state<PayloadBytes>::response_slot_t;

  s.plan = plan;

  sn::util::log::ensuref(
      s.plan.router.batch_size == s.plan.layout.requests_per_client_batch,
      "scooby-omap lb: router batch_size (%zu) != requests_per_client_batch (%llu)", s.plan.router.batch_size,
      static_cast<unsigned long long>(s.plan.layout.requests_per_client_batch)
  );
  sn::util::log::ensuref(
      s.plan.router.bin_capacity == s.plan.layout.bin_capacity,
      "scooby-omap lb: router bin_capacity (%zu) != layout bin_capacity (%llu)", s.plan.router.bin_capacity,
      static_cast<unsigned long long>(s.plan.layout.bin_capacity)
  );

  typename router_types<PayloadBytes>::config cfg{};
  cfg.batch_size = s.plan.router.batch_size;
  cfg.suboram_count = s.plan.router.suboram_count;
  cfg.security_parameter_lambda = s.plan.router.lambda;
  cfg.invalid_key = invalid_key;

  const std::size_t logical_threads = workers.logical_threads();
  auto& pool = workers.pool();
  s.router = std::make_unique<typename engine_state<PayloadBytes>::lbrouter_t>(
      cfg, sn::threads::thread_team(pool, logical_threads)
  );
  s.router->initialize();
  s.selector = std::make_unique<routing_selector<PayloadBytes>>(
      s.plan.suboram_count, sn::threads::thread_team(pool, logical_threads)
  );

  s.router_batch.assign(
      static_cast<std::size_t>(s.plan.router.batch_size),
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>{}
  );
  s.router_reassembled.assign(
      static_cast<std::size_t>(s.plan.router.batch_size),
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>{}
  );
  s.assembled.assign(static_cast<std::size_t>(s.plan.layout.requests_per_client_batch), response_slot_t{});
  s.epoch_state = epoch_ctx<PayloadBytes>(s.plan);

  const std::size_t batch_bytes = static_cast<std::size_t>(s.plan.layout.batch_payload_bytes);
  const std::size_t bin_bytes = static_cast<std::size_t>(s.plan.layout.bin_payload_bytes);
  secure::configure_load_balancer_sessions(
      s.plan, batch_bytes, bin_bytes, s.client_rx_session, s.client_tx_session, s.bin_rx_session, s.bin_tx_session
  );
  s.routed_valid = false;
}

template <std::size_t PayloadBytes> inline void reset_for_epoch(engine_state<PayloadBytes>& s) {
  s.epoch_state.reset_clients();
  s.epoch_state.reset_bins();
  s.routed_valid = false;
}

template <std::size_t PayloadBytes>
inline bool ingest_client_batch(
    engine_state<PayloadBytes>& s, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire,
    std::uint32_t& client_id_out
) {
  global_header envelope{};
  batch_request_header header{};
  std::uint32_t client_id = 0;
  sn::util::span<const std::uint8_t> payload{};
  const bool decoded = protocol::load_balancer::decode_batch_request<PayloadBytes>(
      s.client_rx_session, s.plan, epoch, wire, envelope, header, client_id, payload
  );
  if (!decoded) {
    return false;
  }
  auto& client_ctx = s.epoch_state.clients[client_id];
  sn::util::log::ensuref(
      !client_ctx.seen, "scooby-omap lb: duplicate client batch client=%u epoch=%llu", client_id,
      static_cast<unsigned long long>(epoch)
  );
  decode_slots(payload, client_ctx.slots);
  client_ctx.client_id = client_id;
  client_ctx.seen = true;
  client_id_out = client_id;
  s.routed_valid = false;
  return true;
}

template <std::size_t PayloadBytes>
inline bool ingest_bin_response(
    engine_state<PayloadBytes>& s, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire,
    std::uint32_t& client_id_out, std::uint32_t& suboram_id_out
) {
  global_header envelope{};
  bin_response_header header{};
  std::uint32_t client_id = 0;
  std::uint32_t suboram_id = 0;
  sn::util::span<const std::uint8_t> payload{};
  const bool decoded = protocol::load_balancer::decode_bin_response<PayloadBytes>(
      s.bin_rx_session, s.plan, epoch, wire, envelope, header, client_id, suboram_id, payload
  );
  if (!decoded) {
    return false;
  }
  auto& bin_ctx = s.epoch_state.bin_ctx(client_id, suboram_id, s.plan);
  sn::util::log::ensuref(
      !bin_ctx.seen, "scooby-omap lb: duplicate bin response client=%u suboram=%u epoch=%llu", client_id, suboram_id,
      static_cast<unsigned long long>(epoch)
  );
  decode_slots(payload, bin_ctx.slots);
  bin_ctx.seen = true;
  client_id_out = client_id;
  suboram_id_out = suboram_id;
  return true;
}

template <std::size_t PayloadBytes> inline void route_for_client(engine_state<PayloadBytes>& s, std::uint32_t client) {
  using request_slot_t = typename engine_state<PayloadBytes>::request_slot_t;
  using routed_slot_t = typename engine_state<PayloadBytes>::routed_slot_t;
  using maybe_dummy_request_t =
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>;

  auto& ctx = s.epoch_state.clients[client];
  sn::util::log::ensuref(ctx.seen, "scooby-omap lb: missing client batch client=%u", client);
  const std::size_t batch_size = static_cast<std::size_t>(s.plan.router.batch_size);
  sn::util::log::ensuref(ctx.slots.size() == batch_size, "scooby-omap lb: client batch size mismatch");

  s.routed_batch.resize(batch_size);
  s.selector->assign(
      sn::util::span<const request_slot_t>(ctx.slots.data(), batch_size),
      sn::util::span<routed_slot_t>(s.routed_batch.data(), s.routed_batch.size())
  );
  for (std::size_t ix = 0; ix < batch_size; ++ix) {
    const auto& wire = s.routed_batch[ix];
    auto& entry = s.router_batch[ix];
    entry.is_dummy = wire.is_dummy();
    entry.value.key = wire.key;
    entry.value.suboram_index = wire.suboram_index;
    entry.value.is_write = wire.is_write();
    sn::obliv::copy_n(wire.payload.begin(), PayloadBytes, entry.value.payload.begin());
  }

  s.router->ingest_batch(sn::util::span<const maybe_dummy_request_t>(s.router_batch.data(), batch_size));
  s.router->route();
}

template <std::size_t PayloadBytes>
inline std::size_t build_bin_dispatch(
    engine_state<PayloadBytes>& s, std::uint64_t epoch, std::uint32_t client_id, std::uint32_t suboram_id,
    sn::util::span<std::uint8_t> out_wire
) {
  using routed_slot_t = typename engine_state<PayloadBytes>::routed_slot_t;

  sn::util::log::ensuref(suboram_id < s.plan.suboram_count, "scooby-omap lb: invalid suboram id=%u", suboram_id);

  if (!s.routed_valid || s.routed_epoch != epoch || s.routed_client != client_id) {
    route_for_client(s, client_id);
    s.routed_epoch = epoch;
    s.routed_client = client_id;
    s.routed_valid = true;
  }

  const auto bin_view = s.router->bin_view(suboram_id);
  const std::uint32_t lb_id = s.plan.role_index;
  const std::size_t wire_bytes = protocol::load_balancer::build_bin_dispatch<PayloadBytes>(
      s.bin_tx_session, s.plan, epoch, lb_id, suboram_id, client_id, out_wire,
      [&](sn::util::span<std::uint8_t> payload) {
        const auto slot_bytes = slot_size<routed_slot_t>();
        sn::util::log::ensuref(
            payload.size() == slot_bytes * static_cast<std::size_t>(s.plan.layout.bin_capacity),
            "scooby-omap lb: bin payload size mismatch payload=%zu slots=%llu", payload.size(),
            static_cast<unsigned long long>(s.plan.layout.bin_capacity)
        );
        std::size_t offset = 0;
        for (std::size_t slot_ix = 0; slot_ix < static_cast<std::size_t>(s.plan.layout.bin_capacity);
             ++slot_ix, offset += slot_bytes) {
          routed_slot_t wire{};
          if (slot_ix < bin_view.size()) {
            wire = make_wire_slot<PayloadBytes>(suboram_id, bin_view[slot_ix]);
          } else {
            wire.source_index = static_cast<std::uint32_t>(slot_ix);
            wire.key = invalid_key;
            wire.suboram_index = suboram_id;
            wire.flags = 0;
            wire.set_dummy(true);
            wire.set_write(false);
            zero_payload(wire);
          }
          std::uint8_t* dst = payload.data() + offset;
          sn::obliv::memcpy(dst, &wire, slot_bytes);
        }
      }
  );
  return wire_bytes;
}

template <std::size_t PayloadBytes>
inline void stage_router_responses_for_client(
    engine_state<PayloadBytes>& s, std::uint32_t client, std::uint64_t epoch
) {
  for (std::uint32_t suboram = 0; suboram < s.plan.suboram_count; ++suboram) {
    auto& bin_ctx = s.epoch_state.bin_ctx(client, suboram, s.plan);
    sn::util::log::ensuref(
        bin_ctx.seen, "scooby-omap lb: missing bin response client=%u suboram=%u epoch=%llu", client, suboram,
        static_cast<unsigned long long>(epoch)
    );

    auto bin_view = s.router->bin_view(suboram);
    sn::util::log::ensuref(
        bin_ctx.slots.size() == bin_view.size(),
        "scooby response", client, suboram,
        static_cast<std::size_t>(bin_view.size()), static_cast<std::size_t>(bin_ctx.slots.size())
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
inline void reassemble_for_client(engine_state<PayloadBytes>& s, std::uint32_t client, std::uint64_t epoch) {
  using maybe_dummy_request_t =
      typename router_types<PayloadBytes>::template maybe_dummy<typename router_types<PayloadBytes>::request>;

  const std::size_t batch_size = static_cast<std::size_t>(s.plan.router.batch_size);
  if (!s.routed_valid || s.routed_epoch != epoch || s.routed_client != client) {
    route_for_client(s, client);
    s.routed_epoch = epoch;
    s.routed_client = client;
    s.routed_valid = true;
  }

  stage_router_responses_for_client(s, client, epoch);
  s.router->reassemble(
      sn::util::span<const maybe_dummy_request_t>(s.router_batch.data(), batch_size),
      sn::util::span<maybe_dummy_request_t>(s.router_reassembled.data(), s.router_reassembled.size())
  );

  sn::util::log::ensuref(s.assembled.size() == batch_size, "scooby-omap lb: assembled size mismatch");
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

template <std::size_t PayloadBytes>
inline std::size_t build_client_response(
    engine_state<PayloadBytes>& s, std::uint64_t epoch, std::uint32_t client_id, sn::util::span<std::uint8_t> out_wire
) {
  using response_slot_t = typename engine_state<PayloadBytes>::response_slot_t;

  reassemble_for_client(s, client_id, epoch);

  const std::uint32_t lb_id = s.plan.role_index;
  const std::size_t wire_bytes = protocol::load_balancer::build_batch_response<PayloadBytes>(
      s.client_tx_session, s.plan, epoch, lb_id, client_id,
      sn::util::span<const response_slot_t>(s.assembled.data(), s.assembled.size()), out_wire
  );
  return wire_bytes;
}

}
