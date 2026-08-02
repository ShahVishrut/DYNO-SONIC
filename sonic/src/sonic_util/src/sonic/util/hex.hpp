#pragma once

#include "sonic/util/span.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sn::util::hex {

namespace detail {

inline int decode_nibble(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + (c - 'a');
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + (c - 'A');
  }
  return -1;
}

inline char encode_nibble(std::uint8_t value, bool uppercase) noexcept {
  const char base = uppercase ? 'A' : 'a';
  if (value < 10) {
    return static_cast<char>('0' + value);
  }
  return static_cast<char>(base + (value - 10));
}

}

inline bool is_hex_digit(char c) noexcept { return detail::decode_nibble(c) >= 0; }

inline bool try_decode(std::string_view text, std::vector<std::uint8_t>& out) noexcept {
  if ((text.size() & 1U) != 0U) {
    return false;
  }
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const int hi = detail::decode_nibble(text[i]);
    const int lo = detail::decode_nibble(text[i + 1]);
    if (hi < 0 || lo < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

inline std::vector<std::uint8_t> decode(std::string_view text) {
  std::vector<std::uint8_t> out;
  if (!try_decode(text, out)) {
    throw std::invalid_argument("hex decode requires even-length hexadecimal input");
  }
  return out;
}

inline std::string encode(sn::util::span<const std::uint8_t> bytes, bool uppercase = false) {
  std::string text;
  text.resize(bytes.size() * 2);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const std::uint8_t byte = bytes[i];
    text[(i * 2) + 0] = detail::encode_nibble(static_cast<std::uint8_t>(byte >> 4U), uppercase);
    text[(i * 2) + 1] = detail::encode_nibble(static_cast<std::uint8_t>(byte & 0x0FU), uppercase);
  }
  return text;
}

inline std::string encode(const std::vector<std::uint8_t>& bytes, bool uppercase = false) {
  return encode(sn::util::span<const std::uint8_t>(bytes), uppercase);
}

}
