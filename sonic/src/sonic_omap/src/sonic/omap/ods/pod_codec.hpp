#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "sonic/obliv/ops/platform_ops.hpp"

namespace sn::omap::ods {

// decode bytes to pod type
template <typename T, std::size_t BlockBytes> [[nodiscard]] inline T decode(const std::uint8_t* in) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "ods::decode requires trivially copyable record type");
  static_assert(sizeof(T) <= BlockBytes, "ods::decode record does not fit in ORAM block");

  T out{};
  sn::obliv::memcpy(static_cast<void*>(&out), static_cast<const void*>(in), sizeof(T));
  return out;
}

// encode pod type to bytes
template <typename T, std::size_t BlockBytes> inline void encode(std::uint8_t* out, const T& value) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "ods::encode requires trivially copyable record type");
  static_assert(sizeof(T) <= BlockBytes, "ods::encode record does not fit in ORAM block");

  sn::obliv::memset(static_cast<void*>(out), 0, BlockBytes);
  sn::obliv::memcpy(static_cast<void*>(out), static_cast<const void*>(&value), sizeof(T));
}

} // namespace sn::omap::ods
