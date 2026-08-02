#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#if !defined(SONIC_CRYPTO_TEE)
#include <atomic>
#endif

#include "sonic/crypto/error.hpp"

namespace sn::crypto::detail {

[[noreturn]] inline void throw_openssl_error(const char* context) { throw error(context); }

inline void check_evp(int rc, const char* context) {
  if (rc != 0) {
    throw error(context);
  }
}

inline void check_md(int rc, const char* context) {
  if (rc != 0) {
    throw error(context);
  }
}

inline void secure_zero(void* ptr, std::size_t len) {
#if defined(SONIC_CRYPTO_TEE)
  (void) ptr;
  (void) len;
#else
  if (len == 0) {
    return;
  }

  volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(ptr);
  while (len--) {
    *p++ = 0;
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

template <typename T> inline T load_trivial(const std::uint8_t* src) {
  static_assert(std::is_trivially_copyable<T>::value, "load_trivial requires trivially copyable type");
  T value;
  std::memcpy(&value, src, sizeof(T));
  return value;
}

template <typename T> inline void store_trivial(std::uint8_t* dst, const T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "store_trivial requires trivially copyable type");
  std::memcpy(dst, &value, sizeof(T));
}

constexpr std::size_t max_chunk_size = 1u << 20;

inline std::uint32_t load_be32(const std::uint8_t* src) {
  return (static_cast<std::uint32_t>(src[0]) << 24) | (static_cast<std::uint32_t>(src[1]) << 16) |
         (static_cast<std::uint32_t>(src[2]) << 8) | static_cast<std::uint32_t>(src[3]);
}

inline void store_be32(std::uint8_t* dst, std::uint32_t value) {
  dst[0] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
  dst[1] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  dst[2] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  dst[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

inline void ensure_equal(std::size_t lhs, std::size_t rhs, const char* label) {
  if (lhs != rhs) {
    throw error(label);
  }
}

}
