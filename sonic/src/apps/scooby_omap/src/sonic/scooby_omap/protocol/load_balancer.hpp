#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/scooby_omap/config/plan.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"
#include "sonic/scooby_omap/wire/secure_flow.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::scooby::omap::protocol::load_balancer {

template <std::size_t PayloadBytes>
inline bool decode_batch_request(
    secure_session_t& rx_session, const plan_config& plan, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire,
    global_header& envelope_out, batch_request_header& header_out, std::uint32_t& client_id_out,
    sn::util::span<const std::uint8_t>& payload_out
) {
  return secure::with_decoded_message<batch_request_header>(
      rx_session, wire, message_kind::client_batch_request,
      [&](const global_header& envelope, const batch_request_header& header,
          sn::util::span<const std::uint8_t> payload) {
        sn::util::log::ensuref(
            envelope.epoch_id == epoch, "scooby-omap lb client batch epoch mismatch got=%llu expected=%llu",
            static_cast<unsigned long long>(envelope.epoch_id), static_cast<unsigned long long>(epoch)
        );
        const std::uint32_t client_id = envelope.src_logical_id;
        sn::util::log::ensuref(
            client_id < plan.client_count, "scooby client", client_id
        );
        sn::util::log::ensuref(
            payload.size() == header.payload_bytes && payload.size() == plan.layout.batch_payload_bytes,
            "scooby payload", payload.size(),
            header.payload_bytes, static_cast<unsigned long long>(plan.layout.batch_payload_bytes)
        );
        sn::util::log::ensuref(
            header.request_count == plan.layout.requests_per_client_batch,
            "scooby request", header.request_count,
            static_cast<unsigned long long>(plan.layout.requests_per_client_batch)
        );
        envelope_out = envelope;
        header_out = header;
        client_id_out = client_id;
        payload_out = payload;
      }
  );
}

template <std::size_t PayloadBytes>
inline std::size_t build_bin_dispatch(
    secure_session_t& tx_session, const plan_config& plan, std::uint64_t epoch, std::uint32_t lb_id,
    std::uint32_t suboram_id, std::uint32_t client_id, sn::util::span<const routed_slot<key_type, PayloadBytes>> slots,
    sn::util::span<std::uint8_t> out_wire
) {
  sn::util::log::ensuref(
      slots.size() == static_cast<std::size_t>(plan.layout.bin_capacity), "scooby-omap lb bin slots=%zu expected=%llu",
      slots.size(), static_cast<unsigned long long>(plan.layout.bin_capacity)
  );
  const secure::message_descriptor desc{
      message_kind::lb_bin_dispatch, role_id::load_balancer, role_id::suboram, lb_id, suboram_id, epoch
  };
  return secure::build_message<bin_dispatch_header>(
      tx_session, out_wire, desc, static_cast<std::size_t>(plan.layout.bin_payload_bytes),
      [&](const global_header&, bin_dispatch_header& header, sn::util::span<std::uint8_t> payload) {
        header.client_id = client_id;
        header.lb_id = lb_id;
        header.suboram_id = suboram_id;
        header.slot_count = static_cast<std::uint32_t>(plan.layout.bin_capacity);
        encode_slots(payload, slots);
      }
  );
}

template <std::size_t PayloadBytes, typename PayloadWriter>
inline std::size_t build_bin_dispatch(
    secure_session_t& tx_session, const plan_config& plan, std::uint64_t epoch, std::uint32_t lb_id,
    std::uint32_t suboram_id, std::uint32_t client_id, sn::util::span<std::uint8_t> out_wire,
    PayloadWriter&& payload_writer
) {
  const secure::message_descriptor desc{
      message_kind::lb_bin_dispatch, role_id::load_balancer, role_id::suboram, lb_id, suboram_id, epoch
  };
  return secure::build_message<bin_dispatch_header>(
      tx_session, out_wire, desc, static_cast<std::size_t>(plan.layout.bin_payload_bytes),
      [&](const global_header&, bin_dispatch_header& header, sn::util::span<std::uint8_t> payload) {
        header.client_id = client_id;
        header.lb_id = lb_id;
        header.suboram_id = suboram_id;
        header.slot_count = static_cast<std::uint32_t>(plan.layout.bin_capacity);
        payload_writer(payload);
      }
  );
}

template <std::size_t PayloadBytes>
inline bool decode_bin_response(
    secure_session_t& rx_session, const plan_config& plan, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire,
    global_header& envelope_out, bin_response_header& header_out, std::uint32_t& client_id_out,
    std::uint32_t& suboram_id_out, sn::util::span<const std::uint8_t>& payload_out
) {
  return secure::with_decoded_message<bin_response_header>(
      rx_session, wire, message_kind::suboram_bin_response,
      [&](const global_header& envelope, const bin_response_header& header,
          sn::util::span<const std::uint8_t> payload) {
        sn::util::log::ensuref(
            envelope.epoch_id == epoch, "scooby-omap lb bin response epoch mismatch got=%llu expected=%llu",
            static_cast<unsigned long long>(envelope.epoch_id), static_cast<unsigned long long>(epoch)
        );
        sn::util::log::ensuref(
            header.client_id < plan.client_count && header.suboram_id < plan.suboram_count,
            "scooby-omap lb bin response invalid ids client=%u suboram=%u", header.client_id, header.suboram_id
        );
        envelope_out = envelope;
        header_out = header;
        client_id_out = header.client_id;
        suboram_id_out = header.suboram_id;
        payload_out = payload;
      }
  );
}

template <std::size_t PayloadBytes>
inline std::size_t build_batch_response(
    secure_session_t& tx_session, const plan_config& plan, std::uint64_t epoch, std::uint32_t lb_id,
    std::uint32_t client_id, sn::util::span<const response_slot<key_type, PayloadBytes>> slots,
    sn::util::span<std::uint8_t> out_wire
) {
  sn::util::log::ensuref(
      slots.size() == static_cast<std::size_t>(plan.layout.requests_per_client_batch),
      "scooby-omap lb response slots=%zu expected=%llu", slots.size(),
      static_cast<unsigned long long>(plan.layout.requests_per_client_batch)
  );
  const secure::message_descriptor desc{
      message_kind::lb_batch_response, role_id::load_balancer, role_id::client, lb_id, client_id, epoch
  };
  return secure::build_message<batch_response_header>(
      tx_session, out_wire, desc, static_cast<std::size_t>(plan.layout.batch_payload_bytes),
      [&](const global_header&, batch_response_header& header, sn::util::span<std::uint8_t> payload) {
        header.client_id = client_id;
        header.request_count = static_cast<std::uint32_t>(plan.layout.requests_per_client_batch);
        header.payload_bytes = static_cast<std::uint32_t>(plan.layout.batch_payload_bytes);
        encode_slots(payload, slots);
      }
  );
}

}
