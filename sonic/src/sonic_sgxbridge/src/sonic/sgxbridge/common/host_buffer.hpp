#pragma once

#include <cstdint>
#include <cstddef>

namespace sn::sgxbridge::hostbuf {

using buffer_id = std::uint64_t;

enum class status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  not_found = 2,
  limit_reached = 3,
  internal_error = 4,
};

struct result {
  status code{status::internal_error};
  std::uint32_t detail{0};

  [[nodiscard]] constexpr bool succeeded() const noexcept { return code == status::ok; }
  static constexpr result ok() noexcept { return result{status::ok, 0}; }
};

struct descriptor {
  buffer_id id{0};
  std::uint8_t* data{nullptr};
  std::size_t size{0};
};

[[nodiscard]] constexpr const char* describe(status value) noexcept {
  switch (value) {
  case status::ok:
    return "ok";
  case status::invalid_arguments:
    return "invalid arguments";
  case status::not_found:
    return "not found";
  case status::limit_reached:
    return "limit reached";
  case status::internal_error:
    return "internal error";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::uint32_t to_u32(status value) noexcept { return static_cast<std::uint32_t>(value); }

[[nodiscard]] constexpr status from_u32(std::uint32_t value) noexcept { return static_cast<status>(value); }

}
