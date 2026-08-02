#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/config/types.hpp"
#include "sonic/scooby_omap/wire/slots.hpp"

namespace sn::scooby::omap::client {

template <std::size_t PayloadBytes>
inline void populate_request_slots(
    std::uint32_t suboram_block_count, sn::crypto::buffered_prng<>& prng,
    sn::util::span<request_slot<key_type, PayloadBytes>> slots
) {
  const auto block_count = std::max<std::uint32_t>(1U, suboram_block_count);
  for (std::size_t ix = 0; ix < slots.size(); ++ix) {
    auto& slot = slots[ix];
    slot.source_index = static_cast<std::uint32_t>(ix);
    slot.flags = 0;
    slot.set_dummy(false);
    slot.set_write(false);
    const auto drawn = block_count == 1 ? 0 : static_cast<std::uint32_t>(prng.random_u64() % block_count);
    slot.key = static_cast<key_type>(drawn);
    prng.random_bytes(slot.payload.data(), slot.payload.size());
  }
}

}
