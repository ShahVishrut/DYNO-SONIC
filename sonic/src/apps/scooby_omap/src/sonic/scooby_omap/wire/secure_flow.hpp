#pragma once

#include "sonic/scooby_omap/wire/secure.hpp"

namespace sn::scooby::omap::secure {

struct message_descriptor {
  message_kind kind;
  role_id src_role;
  role_id dst_role;
  std::uint32_t src_id;
  std::uint32_t dst_id;
  std::uint64_t epoch_id;
};

template <typename Header, typename Writer>
inline std::size_t build_message(
    secure_session_t& session, sn::util::span<std::uint8_t> writable, const message_descriptor& desc,
    std::size_t payload_bytes, Writer&& writer
) {
  secure_builder_t builder(session, writable);
  auto view = prepare_secure_message<Header>(
      builder, desc.kind, desc.src_role, desc.dst_role, desc.src_id, desc.dst_id, desc.epoch_id, payload_bytes
  );
  writer(*view.global, *view.specific, view.payload);
  builder.finalize();
  return view.total_wire_bytes;
}

template <typename Header, typename Handler>
inline bool with_decoded_message(
    secure_session_t& session, sn::util::span<const std::uint8_t> bytes, message_kind expected_kind, Handler&& handler
) {
  global_header envelope{};
  Header header{};
  sn::util::span<const std::uint8_t> payload;
  if (!decode_secure_message(session, bytes, expected_kind, envelope, header, payload)) {
    return false;
  }
  handler(envelope, header, payload);
  return true;
}

}
