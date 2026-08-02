#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::scooby::omap {

inline constexpr std::uint32_t wire_magic = 0x736E6963u;
inline constexpr std::uint16_t wire_version = 1u;

enum class role_id : std::uint8_t {
  client = 1,
  load_balancer = 2,
  suboram = 3,
};

enum class message_kind : std::uint8_t {
  client_batch_request = 1,
  lb_bin_dispatch = 2,
  suboram_bin_response = 3,
  lb_batch_response = 4,
};

struct global_header {
  std::uint32_t magic{wire_magic};
  std::uint16_t version{wire_version};
  std::uint16_t header_bytes{static_cast<std::uint16_t>(sizeof(global_header))};
  std::uint64_t epoch_id{0};
  std::uint32_t src_logical_id{0};
  std::uint32_t dst_logical_id{0};
  std::uint8_t src_role{static_cast<std::uint8_t>(role_id::client)};
  std::uint8_t dst_role{static_cast<std::uint8_t>(role_id::load_balancer)};
  std::uint8_t kind{static_cast<std::uint8_t>(message_kind::client_batch_request)};
  std::uint8_t reserved0{0};
  std::uint32_t reserved1{0};
};
static_assert(sizeof(global_header) == 32, "global_header must remain 32 bytes");

struct batch_request_header {
  std::uint32_t lb_count{0};
  std::uint32_t request_count{0};
  std::uint32_t payload_bytes{0};
  std::uint32_t reserved{0};
};

struct bin_dispatch_header {
  std::uint32_t client_id{0};
  std::uint32_t lb_id{0};
  std::uint32_t suboram_id{0};
  std::uint32_t slot_count{0};
};

struct bin_response_header {
  std::uint32_t client_id{0};
  std::uint32_t lb_id{0};
  std::uint32_t suboram_id{0};
  std::uint32_t slot_count{0};
};

struct batch_response_header {
  std::uint32_t client_id{0};
  std::uint32_t request_count{0};
  std::uint32_t payload_bytes{0};
  std::uint32_t reserved{0};
};

inline constexpr std::size_t global_header_bytes() noexcept { return sizeof(global_header); }
inline constexpr std::size_t batch_request_header_bytes() noexcept { return sizeof(batch_request_header); }
inline constexpr std::size_t bin_dispatch_header_bytes() noexcept { return sizeof(bin_dispatch_header); }
inline constexpr std::size_t bin_response_header_bytes() noexcept { return sizeof(bin_response_header); }
inline constexpr std::size_t batch_response_header_bytes() noexcept { return sizeof(batch_response_header); }

inline role_id decode_role(std::uint8_t raw) { return static_cast<role_id>(raw); }

inline message_kind decode_kind(std::uint8_t raw) { return static_cast<message_kind>(raw); }

inline role_id header_src_role(const global_header& header) { return decode_role(header.src_role); }

inline role_id header_dst_role(const global_header& header) { return decode_role(header.dst_role); }

inline message_kind header_kind(const global_header& header) { return decode_kind(header.kind); }

inline std::size_t specific_header_bytes(const global_header& header) {
  return static_cast<std::size_t>(header.header_bytes) - sizeof(global_header);
}

inline std::size_t total_message_bytes(const global_header& header, std::size_t payload_bytes) {
  return static_cast<std::size_t>(header.header_bytes) + payload_bytes;
}

inline global_header make_global_header(
    message_kind kind, role_id src_role, role_id dst_role, std::uint32_t src_id, std::uint32_t dst_id,
    std::uint64_t epoch_id, std::uint16_t header_bytes
) {
  global_header gh{};
  gh.header_bytes = header_bytes;
  gh.kind = static_cast<std::uint8_t>(kind);
  gh.src_role = static_cast<std::uint8_t>(src_role);
  gh.dst_role = static_cast<std::uint8_t>(dst_role);
  gh.src_logical_id = src_id;
  gh.dst_logical_id = dst_id;
  gh.epoch_id = epoch_id;
  return gh;
}

inline bool decode_global_header(global_header& header, sn::util::span<const std::uint8_t> bytes) {
  if (bytes.size() < sizeof(global_header)) {
    return false;
  }
  sn::obliv::memcpy(&header, bytes.data(), sizeof(global_header));
  return header.magic == wire_magic && header.version == wire_version && header.header_bytes >= sizeof(global_header);
}

inline sn::util::span<const std::uint8_t> specific_header_span(
    const global_header& header, sn::util::span<const std::uint8_t> bytes
) {
  const auto header_bytes = static_cast<std::size_t>(header.header_bytes);
  sn::util::log::ensuref(bytes.size() >= header_bytes, "schema: payload shorter than header bytes");
  return bytes.subspan(sizeof(global_header), header_bytes - sizeof(global_header));
}

inline sn::util::span<const std::uint8_t> payload_span(
    const global_header& header, sn::util::span<const std::uint8_t> bytes
) {
  const auto header_bytes = static_cast<std::size_t>(header.header_bytes);
  sn::util::log::ensuref(bytes.size() >= header_bytes, "schema: payload shorter than header bytes");
  return bytes.subspan(header_bytes, bytes.size() - header_bytes);
}

template <typename Header> struct message_builder {
  global_header* global{nullptr};
  Header* specific{nullptr};
  sn::util::span<std::uint8_t> payload{};

  std::size_t total_bytes() const noexcept { return sizeof(Header) + sizeof(global_header) + payload.size(); }
};

template <typename Header>
inline message_builder<Header> prepare_message(
    sn::util::span<std::uint8_t> buffer, message_kind kind, role_id src_role, role_id dst_role, std::uint32_t src_id,
    std::uint32_t dst_id, std::uint64_t epoch_id, std::size_t payload_bytes
) {
  const auto required_header = sizeof(global_header) + sizeof(Header);
  sn::util::log::ensuref(
      buffer.size() >= required_header + payload_bytes,
      "schema: buffer (%zu) too small for header (%zu) + payload (%zu)", buffer.size(), required_header, payload_bytes
  );
  const auto buffer_addr = reinterpret_cast<std::uintptr_t>(buffer.data());
  sn::util::log::ensuref(
      (buffer_addr % alignof(global_header)) == 0, "schema: buffer %p is misaligned for global_header", buffer.data()
  );
  auto* gh = reinterpret_cast<global_header*>(buffer.data());
  const auto encoded_header = make_global_header(
      kind, src_role, dst_role, src_id, dst_id, epoch_id, static_cast<std::uint16_t>(required_header)
  );
  sn::obliv::memcpy(gh, &encoded_header, sizeof(global_header));
  auto* specific = reinterpret_cast<Header*>(buffer.data() + sizeof(global_header));
  sn::util::log::ensuref(
      (reinterpret_cast<std::uintptr_t>(specific) % alignof(Header)) == 0,
      "schema: buffer %p is misaligned for specific header", specific
  );
  sn::obliv::memset(specific, 0, sizeof(Header));
  auto payload = sn::util::span<std::uint8_t>(buffer.data() + required_header, payload_bytes);
  return {gh, specific, payload};
}

template <typename Header>
inline bool decode_message(
    sn::util::span<const std::uint8_t> bytes, message_kind expected_kind, global_header& header_out,
    Header& specific_out, sn::util::span<const std::uint8_t>& payload_out
) {
  if (!decode_global_header(header_out, bytes)) {
    return false;
  }
  if (header_kind(header_out) != expected_kind) {
    return false;
  }
  const auto header_bytes = static_cast<std::size_t>(header_out.header_bytes);
  if (header_bytes != sizeof(global_header) + sizeof(Header)) {
    return false;
  }
  sn::obliv::memcpy(&specific_out, bytes.data() + sizeof(global_header), sizeof(Header));
  payload_out = payload_span(header_out, bytes);
  return true;
}

}
