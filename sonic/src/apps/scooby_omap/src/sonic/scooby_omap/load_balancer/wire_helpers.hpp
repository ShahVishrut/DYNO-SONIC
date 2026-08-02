#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/scooby_omap/load_balancer/types.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::load_balancer {

template <std::size_t PayloadBytes>
inline routed_slot<key_type, PayloadBytes> make_wire_slot(
    std::uint32_t suboram_id, const typename router_types<PayloadBytes>::routed_slot& entry
) {
  routed_slot<key_type, PayloadBytes> wire{};
  wire.source_index = entry.source_index;
  wire.key = static_cast<key_type>(entry.item.key);
  wire.suboram_index = suboram_id;
  wire.flags = 0;
  wire.set_dummy(entry.is_dummy);
  wire.set_write(entry.item.is_write);
  copy_payload_obliv(entry.item, wire);
  return wire;
}

}
