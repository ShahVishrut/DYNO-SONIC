#pragma once

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/scooby_omap/secure/session.hpp"
#include "sonic/scooby_omap/wire/schema.hpp"

namespace sn::scooby::omap {

template <typename Header> struct secure_message_view {
  global_header* global{nullptr};
  Header* specific{nullptr};
  sn::util::span<std::uint8_t> payload{};
  std::size_t total_wire_bytes{0};
};

template <typename Header>
inline secure_message_view<Header> prepare_secure_message(
    secure_builder_t& builder, message_kind kind, role_id src_role, role_id dst_role, std::uint32_t src_id,
    std::uint32_t dst_id, std::uint64_t epoch_id, std::size_t payload_bytes
) {
  const auto required_header = sizeof(global_header) + sizeof(Header);
  auto header_span = builder.header_buffer(required_header);
  const auto header_addr = reinterpret_cast<std::uintptr_t>(header_span.data());
  sn::util::log::ensuref(
      (header_addr % alignof(global_header)) == 0, "secure schema: buffer %p misaligned for global_header",
      header_span.data()
  );
  auto* gh = reinterpret_cast<global_header*>(header_span.data());
  const auto encoded_header = make_global_header(
      kind, src_role, dst_role, src_id, dst_id, epoch_id, static_cast<std::uint16_t>(required_header)
  );
  sn::obliv::memcpy(gh, &encoded_header, sizeof(global_header));
  auto* specific = reinterpret_cast<Header*>(header_span.data() + sizeof(global_header));
  sn::util::log::ensuref(
      (reinterpret_cast<std::uintptr_t>(specific) % alignof(Header)) == 0,
      "secure schema: buffer %p misaligned for header", specific
  );
  sn::obliv::memset(specific, 0, sizeof(Header));
  auto payload_span = builder.payload_buffer(payload_bytes);
  return secure_message_view<Header>{gh, specific, payload_span, builder.total_wire_bytes()};
}

template <typename Header>
inline bool decode_secure_message(
    secure_session_t& session, sn::util::span<const std::uint8_t> bytes, message_kind expected_kind,
    global_header& header_out, Header& specific_out, sn::util::span<const std::uint8_t>& payload_out
) {
  const std::size_t header_bytes = sizeof(global_header) + sizeof(Header);
  sn::util::span<const std::uint8_t> header_view;
  sn::util::span<const std::uint8_t> payload_view;
  if (!secure_builder_t::decrypt_into(session, bytes, header_bytes, header_view, payload_view)) {
    return false;
  }
  sn::obliv::memcpy(&header_out, header_view.data(), sizeof(global_header));
  if (header_kind(header_out) != expected_kind) {
    return false;
  }
  sn::obliv::memcpy(&specific_out, header_view.data() + sizeof(global_header), sizeof(Header));
  payload_out = payload_view;
  return true;
}

}
