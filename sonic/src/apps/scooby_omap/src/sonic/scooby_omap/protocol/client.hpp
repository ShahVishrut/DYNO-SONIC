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

namespace sn::scooby::omap::protocol::client {

template <std::size_t PayloadBytes>
inline std::size_t build_batch_request(
    secure_session_t& tx_session, const plan_config& plan, std::uint64_t epoch, std::uint32_t client_id,
    std::uint32_t lb_id, sn::util::span<const request_slot<key_type, PayloadBytes>> slots,
    sn::util::span<std::uint8_t> out_wire
) {
  sn::util::log::ensuref(
      slots.size() == static_cast<std::size_t>(plan.layout.requests_per_client_batch),
      "scooby-omap client request slots=%zu expected=%llu", slots.size(),
      static_cast<unsigned long long>(plan.layout.requests_per_client_batch)
  );
  const secure::message_descriptor desc{
      message_kind::client_batch_request, role_id::client, role_id::load_balancer, client_id, lb_id, epoch
  };
  return secure::build_message<batch_request_header>(
      tx_session, out_wire, desc, static_cast<std::size_t>(plan.layout.batch_payload_bytes),
      [&](const global_header&, batch_request_header& header, sn::util::span<std::uint8_t> payload) {
        header.lb_count = plan.load_balancer_count;
        header.request_count = static_cast<std::uint32_t>(plan.layout.requests_per_client_batch);
        header.payload_bytes = static_cast<std::uint32_t>(plan.layout.batch_payload_bytes);
        encode_slots(payload, slots);
      }
  );
}

template <std::size_t PayloadBytes>
inline bool decode_batch_response(
    secure_session_t& rx_session, std::uint64_t epoch, std::uint32_t expected_client_id,
    sn::util::span<const std::uint8_t> wire, global_header& envelope_out, batch_response_header& header_out,
    std::vector<response_slot<key_type, PayloadBytes>>& slots_out
) {
  return secure::with_decoded_message<batch_response_header>(
      rx_session, wire, message_kind::lb_batch_response,
      [&](const global_header& envelope, const batch_response_header& header,
          sn::util::span<const std::uint8_t> payload) {
        sn::util::log::ensuref(
            envelope.epoch_id == epoch, "scooby-omap client response epoch mismatch got=%llu expected=%llu",
            static_cast<unsigned long long>(envelope.epoch_id), static_cast<unsigned long long>(epoch)
        );
        sn::util::log::ensuref(
            header.client_id == expected_client_id, "scooby-omap client response client_id mismatch got=%u expected=%u",
            header.client_id, expected_client_id
        );
        sn::util::log::ensuref(
            payload.size() == header.payload_bytes, "scooby-omap client response payload bytes=%zu expected=%u",
            payload.size(), header.payload_bytes
        );
        envelope_out = envelope;
        header_out = header;
        decode_slots(payload, slots_out);
      }
  );
}

}
