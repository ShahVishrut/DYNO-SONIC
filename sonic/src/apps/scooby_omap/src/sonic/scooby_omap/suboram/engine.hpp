#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/protocol/suboram.hpp"
#include "sonic/scooby_omap/secure/helpers.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/suboram/backend.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::suboram {

template <std::size_t PayloadBytes> struct engine_state {
  using bin_slot_t = routed_slot<key_type, PayloadBytes>;

  plan_config plan{};
  std::unique_ptr<suboram_backend<PayloadBytes>> backend{};
  std::vector<bin_slot_t> bin_slots{};
  secure_session_t bin_rx_session{};
  secure_session_t response_tx_session{};
};

template <std::size_t PayloadBytes, typename BackendT>
inline void configure(engine_state<PayloadBytes>& s, const plan_config& plan, std::unique_ptr<BackendT> backend) {
  static_assert(
      std::is_base_of_v<suboram_backend<PayloadBytes>, BackendT>,
      "suboram backend"
  );
  s.plan = plan;
  s.backend = std::move(backend);
  sn::util::log::ensuref(static_cast<bool>(s.backend), "scooby-omap suboram engine requires a backend");
  sn::util::log::ensuref(
      s.backend->bin_capacity() == static_cast<std::size_t>(s.plan.layout.bin_capacity),
      "scooby-omap suboram backend capacity mismatch got=%zu expected=%llu", s.backend->bin_capacity(),
      static_cast<unsigned long long>(s.plan.layout.bin_capacity)
  );

  s.bin_slots.assign(
      static_cast<std::size_t>(s.plan.layout.bin_capacity), typename engine_state<PayloadBytes>::bin_slot_t{}
  );
  const std::size_t bin_bytes = static_cast<std::size_t>(s.plan.layout.bin_payload_bytes);
  secure::configure_suboram_sessions(s.plan, bin_bytes, s.bin_rx_session, s.response_tx_session);
}

template <std::size_t PayloadBytes>
inline bool process_bin_dispatch(
    engine_state<PayloadBytes>& s, std::uint64_t epoch, sn::util::span<const std::uint8_t> wire_in,
    sn::util::span<std::uint8_t> wire_out, bin_dispatch_header& request_header_out, std::size_t& bytes_written_out
) {
  global_header envelope{};
  bin_dispatch_header request_header{};
  const bool decoded = protocol::suboram::decode_bin_dispatch<PayloadBytes>(
      s.bin_rx_session, s.plan, epoch, wire_in, envelope, request_header, s.bin_slots
  );
  if (!decoded) {
    return false;
  }

  s.backend->process_bin(
      sn::util::span<typename engine_state<PayloadBytes>::bin_slot_t>(s.bin_slots.data(), s.bin_slots.size())
  );
  s.backend->perform_maintenance();

  const std::size_t wire_bytes = protocol::suboram::build_bin_response<PayloadBytes>(
      s.response_tx_session, s.plan, epoch, s.plan.role_index, request_header,
      sn::util::span<const typename engine_state<PayloadBytes>::bin_slot_t>(s.bin_slots.data(), s.bin_slots.size()),
      wire_out
  );
  request_header_out = request_header;
  bytes_written_out = wire_bytes;
  return true;
}

}
