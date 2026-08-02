#pragma once

#include <array>
#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace sn::omap::harness::scooby {

template <std::size_t PayloadBytes> struct payload_tag : std::integral_constant<std::size_t, PayloadBytes> {
  static_assert(PayloadBytes > 0, "scooby payload must be positive");
  static_assert(PayloadBytes % alignof(std::uint64_t) == 0, "scooby payload must be 64-bit aligned");
};

constexpr std::array<std::size_t, 3> kSupportedPayloadBytes{64, 256, 1024};

constexpr bool payload_supported(std::size_t bytes) noexcept {
  for (auto supported : kSupportedPayloadBytes) {
    if (supported == bytes) {
      return true;
    }
  }
  return false;
}

template <typename Fn> decltype(auto) dispatch_payload(std::size_t bytes, Fn&& fn) {
  switch (bytes) {
  case 64:
    return fn(payload_tag<64>{});
  case 256:
    return fn(payload_tag<256>{});
  case 1024:
    return fn(payload_tag<1024>{});
  default:
    throw std::invalid_argument("unsupported scooby payload size");
  }
}

template <typename Fn> void for_each_payload(Fn&& fn) {
  fn(payload_tag<64>{});
  fn(payload_tag<256>{});
  fn(payload_tag<1024>{});
}

} // namespace sn::omap::harness::scooby
