#pragma once

#include <utility>

#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/dist/api.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/wire/schema.hpp"

namespace sn::scooby::omap {

struct inbound_message {
  sn::sgxbridge::dist::scoped_message message{};
  global_header header{};
  sn::util::span<const std::uint8_t> bytes{};
  sn::sgxbridge::time::steady_clock::time_point enqueued_at{};
};

inline bool decode_inbound_message(sn::sgxbridge::dist::scoped_message&& scoped, inbound_message& out) {
  auto payload = scoped.payload();
  if (!decode_global_header(out.header, payload)) {
    return false;
  }
  out.bytes = payload;
  out.message = std::move(scoped);
  return true;
}

inline sn::util::span<const std::uint8_t> message_specific_view(const inbound_message& msg) {
  return specific_header_span(msg.header, msg.bytes);
}

inline sn::util::span<const std::uint8_t> message_payload_view(const inbound_message& msg) {
  return payload_span(msg.header, msg.bytes);
}

}
