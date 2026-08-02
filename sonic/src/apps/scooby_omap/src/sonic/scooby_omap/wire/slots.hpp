#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/scooby_omap/config/types.hpp"

namespace sn::scooby::omap {

inline constexpr std::uint32_t slot_flag_write = 0x01u;
inline constexpr std::uint32_t slot_flag_dummy = 0x02u;

template <typename Slot> constexpr std::size_t slot_size() noexcept { return sizeof(Slot); }

inline std::uint32_t set_flag_ct(std::uint32_t flags, std::uint32_t mask, bool value) noexcept {
  const std::uint32_t with_flag = static_cast<std::uint32_t>(flags | mask);
  const std::uint32_t without_flag = static_cast<std::uint32_t>(flags & ~mask);
  return sn::obliv::ct_select<std::uint32_t>(with_flag, without_flag, value);
}

template <typename Key, std::size_t PayloadBytes> struct slot_payload_base {
  static_assert(PayloadBytes > 0, "slot payload must be positive");
  static_assert(PayloadBytes % alignof(std::uint64_t) == 0, "slot payload must be 64-bit aligned");
  static_assert(std::is_unsigned_v<Key>, "slot key must be an unsigned integral type");

  std::uint32_t source_index{0};
  Key key{};
  std::uint32_t flags{slot_flag_dummy};
  std::array<std::uint8_t, PayloadBytes> payload{};

  [[nodiscard]] bool is_dummy() const noexcept { return (flags & slot_flag_dummy) != 0U; }
  [[nodiscard]] bool is_write() const noexcept { return (flags & slot_flag_write) != 0U; }

  void set_dummy(bool value) noexcept { flags = set_flag_ct(flags, slot_flag_dummy, value); }

  void set_write(bool value) noexcept { flags = set_flag_ct(flags, slot_flag_write, value); }
};

template <typename Key, std::size_t PayloadBytes> struct request_slot : slot_payload_base<Key, PayloadBytes> {};

template <typename Key, std::size_t PayloadBytes> struct routed_slot {
  static_assert(std::is_unsigned_v<Key>, "slot key must be unsigned");

  std::uint32_t source_index{0};
  Key key{};
  std::uint32_t suboram_index{0};
  std::uint32_t flags{slot_flag_dummy};
  std::array<std::uint8_t, PayloadBytes> payload{};

  [[nodiscard]] bool is_dummy() const noexcept { return (flags & slot_flag_dummy) != 0; }
  [[nodiscard]] bool is_write() const noexcept { return (flags & slot_flag_write) != 0; }

  void set_dummy(bool value) noexcept { flags = set_flag_ct(flags, slot_flag_dummy, value); }

  void set_write(bool value) noexcept { flags = set_flag_ct(flags, slot_flag_write, value); }
};

template <typename Key, std::size_t PayloadBytes> using response_slot = request_slot<Key, PayloadBytes>;

template <typename Slot> inline void zero_payload(Slot& slot) noexcept { slot.payload.fill(0); }

template <typename SrcSlot, typename DstSlot>
inline void copy_payload_obliv(const SrcSlot& src, DstSlot& dst) noexcept {
  sn::obliv::copy_n(src.payload.begin(), src.payload.size(), dst.payload.begin());
}

template <typename Slot> inline void encode_slots(sn::util::span<std::uint8_t> dst, sn::util::span<const Slot> slots) {
  static_assert(std::is_trivially_copyable_v<Slot>, "slot encode requires trivially copyable Slot");
  static_assert(std::is_standard_layout_v<Slot>, "slot encode requires standard layout Slot");
  const auto expected = slot_size<Slot>() * slots.size();
  sn::util::log::ensuref(
      dst.size() == expected, "scooby-omap: slot encode size mismatch got=%zu expected=%zu", dst.size(), expected
  );
  if (expected == 0) {
    return;
  }
  sn::obliv::memcpy(dst.data(), slots.data(), expected);
}

template <typename Slot> inline void decode_slots(sn::util::span<const std::uint8_t> src, std::vector<Slot>& out) {
  static_assert(std::is_trivially_copyable_v<Slot>, "slot decode requires trivially copyable Slot");
  static_assert(std::is_standard_layout_v<Slot>, "slot decode requires standard layout Slot");
  const auto bytes = slot_size<Slot>();
  sn::util::log::ensuref(
      src.size() % bytes == 0, "scooby-omap: slot decode payload=%zu not divisible by slot size=%zu", src.size(), bytes
  );
  const std::size_t count = src.size() / bytes;
  out.resize(count);
  if (count == 0) {
    return;
  }
  sn::obliv::memcpy(out.data(), src.data(), src.size());
}

template <typename Key, std::size_t PayloadBytes>
inline void copy_slot(const request_slot<Key, PayloadBytes>& src, request_slot<Key, PayloadBytes>& dst) noexcept {
  dst.source_index = src.source_index;
  dst.key = src.key;
  dst.flags = src.flags;
  sn::obliv::copy_n(src.payload.begin(), PayloadBytes, dst.payload.begin());
}

template <typename Key, std::size_t PayloadBytes>
inline void copy_slot(const request_slot<Key, PayloadBytes>& src, routed_slot<Key, PayloadBytes>& dst) noexcept {
  dst.source_index = src.source_index;
  dst.key = src.key;
  dst.flags = src.flags;
  sn::obliv::copy_n(src.payload.begin(), PayloadBytes, dst.payload.begin());
}

template <typename Key, std::size_t PayloadBytes>
inline void copy_slot(const routed_slot<Key, PayloadBytes>& src, response_slot<Key, PayloadBytes>& dst) noexcept {
  dst.source_index = src.source_index;
  dst.key = src.key;
  dst.flags = src.flags;
  sn::obliv::copy_n(src.payload.begin(), PayloadBytes, dst.payload.begin());
}

static_assert(std::is_trivially_copyable_v<request_slot<std::uint32_t, 64>>, "request_slot must be trivially copyable");
static_assert(std::is_trivially_copyable_v<routed_slot<std::uint32_t, 64>>, "routed_slot must be trivially copyable");

}
