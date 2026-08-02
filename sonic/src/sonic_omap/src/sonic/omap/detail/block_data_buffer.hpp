#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sn::omap::detail {

template <std::size_t BlockBytes> struct alignas(alignof(std::uint64_t)) block_data_buffer {
  std::array<std::uint8_t, BlockBytes> bytes{};

  [[nodiscard]] constexpr std::size_t size() const noexcept { return BlockBytes; }
  [[nodiscard]] std::uint8_t* data() noexcept { return bytes.data(); }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes.data(); }

  void fill(std::uint8_t value) noexcept { bytes.fill(value); }

  std::uint8_t& operator[](std::size_t ix) noexcept { return bytes[ix]; }
  const std::uint8_t& operator[](std::size_t ix) const noexcept { return bytes[ix]; }

  auto begin() noexcept { return bytes.begin(); }
  auto end() noexcept { return bytes.end(); }
  auto begin() const noexcept { return bytes.begin(); }
  auto end() const noexcept { return bytes.end(); }
  auto cbegin() const noexcept { return bytes.cbegin(); }
  auto cend() const noexcept { return bytes.cend(); }
};

} // namespace sn::omap::detail
