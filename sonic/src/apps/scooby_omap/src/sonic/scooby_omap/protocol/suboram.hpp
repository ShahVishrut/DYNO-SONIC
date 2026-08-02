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

namespace sn::scooby::omap::protocol::suboram {

template <std::size_t PayloadBytes>
inline bool decode_bin_dispatch(
    secure_session_t& rx_session, const plan_config& plan, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire,
    global_header& envelope_out, bin_dispatch_header& header_out,
    std::vector<routed_slot<key_type, PayloadBytes>>& slots_out
) {
  return secure::with_decoded_message<bin_dispatch_header>(
      rx_session, wire, message_kind::lb_bin_dispatch,
      [&](const global_header& envelope, const bin_dispatch_header& header,
          sn::util::span<const std::uint8_t> payload) {
        sn::util::log::ensuref(
            envelope.epoch_id == epoch, "scooby-omap suboram bin dispatch epoch mismatch got=%llu expected=%llu",
            static_cast<unsigned long long>(envelope.epoch_id), static_cast<unsigned long long>(epoch)
        );
        sn::util::log::ensuref(
            header.lb_id < plan.load_balancer_count, "scooby-omap suboram invalid lb id=%u", header.lb_id
        );
        sn::util::log::ensuref(
            header.slot_count == plan.layout.bin_capacity, "scooby-omap suboram unexpected slot_count=%u expected=%llu",
            header.slot_count, static_cast<unsigned long long>(plan.layout.bin_capacity)
        );
        envelope_out = envelope;
        header_out = header;
        decode_slots(payload, slots_out);
      }
  );
}

template <std::size_t PayloadBytes>
inline std::size_t build_bin_response(
    secure_session_t& tx_session, const plan_config& plan, std::uint64_t epoch, std::uint32_t suboram_id,
    const bin_dispatch_header& request_header, sn::util::span<const routed_slot<key_type, PayloadBytes>> slots,
    sn::util::span<std::uint8_t> out_wire
) {
  sn::util::log::ensuref(
      slots.size() == static_cast<std::size_t>(plan.layout.bin_capacity),
      "scooby-omap suboram response slots=%zu expected=%llu", slots.size(),
      static_cast<unsigned long long>(plan.layout.bin_capacity)
  );
  const secure::message_descriptor desc{message_kind::suboram_bin_response,
                                        role_id::suboram,
                                        role_id::load_balancer,
                                        suboram_id,
                                        request_header.lb_id,
                                        epoch};
  return secure::build_message<bin_response_header>(
      tx_session, out_wire, desc, static_cast<std::size_t>(plan.layout.bin_payload_bytes),
      [&](const global_header&, bin_response_header& header, sn::util::span<std::uint8_t> payload) {
        header.client_id = request_header.client_id;
        header.lb_id = request_header.lb_id;
        header.suboram_id = suboram_id;
        header.slot_count = request_header.slot_count;
        encode_slots(payload, slots);
      }
  );
}

}
