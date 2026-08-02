#include "sonic/crypto/impl/openssl/prf_backend_lowlevel.hpp"

#if SONIC_OPENSSL_USE_LOWLEVEL

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#if SONIC_OPENSSL_HAS_AESNI
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/openssl/detail/utils.hpp"

namespace sn::crypto::detail {

namespace {

constexpr std::size_t kBlockSize = 16;

#if SONIC_OPENSSL_HAS_AESNI

inline std::uint32_t to_be32(std::uint32_t value) {
#if defined(_MSC_VER)
  return _byteswap_ulong(value);
#else
  return __builtin_bswap32(value);
#endif
}

inline __m128i load_counter(const std::uint8_t* iv, std::uint32_t counter) {
  const __m128i base = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
  const std::uint32_t be_counter = to_be32(counter);
  return _mm_insert_epi32(base, static_cast<int>(be_counter), 3);
}

inline __m128i encrypt_block(__m128i block, const openssl_prf_lowlevel_state& state) {
  const auto* round_keys = reinterpret_cast<const __m128i*>(state.round_keys.data());
  block = _mm_xor_si128(block, round_keys[0]);
  for (int round = 1; round < state.rounds; ++round) {
    block = _mm_aesenc_si128(block, round_keys[round]);
  }
  return _mm_aesenclast_si128(block, round_keys[state.rounds]);
}

#endif

}

void openssl_prf_lowlevel_cleanup(openssl_prf_lowlevel_state& state) {
  detail::secure_zero(&state.key, sizeof(state.key));
#if SONIC_OPENSSL_HAS_AESNI
  detail::secure_zero(state.round_keys.data(), state.round_keys.size());
#endif
  state.rounds = 0;
}

void openssl_prf_lowlevel_set_key(openssl_prf_lowlevel_state& state, sn::util::span<const std::uint8_t> key_bytes) {
  if (key_bytes.size() != 32) {
    throw error("openssl prf key must be 32 bytes");
  }
  if (AES_set_encrypt_key(key_bytes.data(), 256, &state.key) != 0) {
    throw error("AES_set_encrypt_key failed");
  }
  state.rounds = state.key.rounds;
#if SONIC_OPENSSL_HAS_AESNI
  auto* dst = reinterpret_cast<__m128i*>(state.round_keys.data());
  const auto* src = reinterpret_cast<const __m128i*>(state.key.rd_key);
  for (int round = 0; round <= state.rounds; ++round) {
    dst[round] = _mm_loadu_si128(src + round);
  }
#endif
}

void openssl_prf_lowlevel_stream(
    openssl_prf_lowlevel_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len
) {
  if (len == 0) {
    return;
  }

  std::uint32_t counter = 0;

#if SONIC_OPENSSL_HAS_AESNI
  while (len >= 64) {
    const __m128i x0 = encrypt_block(load_counter(iv, counter + 0u), state);
    const __m128i x1 = encrypt_block(load_counter(iv, counter + 1u), state);
    const __m128i x2 = encrypt_block(load_counter(iv, counter + 2u), state);
    const __m128i x3 = encrypt_block(load_counter(iv, counter + 3u), state);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 0), x0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 16), x1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 32), x2);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 48), x3);

    out += 64;
    len -= 64;
    counter += 4;
  }

  while (len >= kBlockSize) {
    const __m128i block = encrypt_block(load_counter(iv, counter), state);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), block);
    out += kBlockSize;
    len -= kBlockSize;
    ++counter;
  }

  if (len > 0) {
    alignas(16) std::array<std::uint8_t, kBlockSize> keystream{};
    const __m128i block = encrypt_block(load_counter(iv, counter), state);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(keystream.data()), block);
    std::memcpy(out, keystream.data(), len);
    detail::secure_zero(keystream.data(), keystream.size());
  }
#else
  std::array<std::uint8_t, kBlockSize> counter_block{};
  std::memcpy(counter_block.data(), iv, kBlockSize);

  while (len >= kBlockSize) {
    counter_block[12] = static_cast<std::uint8_t>(counter >> 24U);
    counter_block[13] = static_cast<std::uint8_t>(counter >> 16U);
    counter_block[14] = static_cast<std::uint8_t>(counter >> 8U);
    counter_block[15] = static_cast<std::uint8_t>(counter);
    AES_encrypt(counter_block.data(), out, &state.key);
    out += kBlockSize;
    len -= kBlockSize;
    ++counter;
  }

  if (len > 0) {
    counter_block[12] = static_cast<std::uint8_t>(counter >> 24U);
    counter_block[13] = static_cast<std::uint8_t>(counter >> 16U);
    counter_block[14] = static_cast<std::uint8_t>(counter >> 8U);
    counter_block[15] = static_cast<std::uint8_t>(counter);
    std::array<std::uint8_t, kBlockSize> keystream{};
    AES_encrypt(counter_block.data(), keystream.data(), &state.key);
    std::memcpy(out, keystream.data(), len);
    detail::secure_zero(keystream.data(), keystream.size());
  }

  detail::secure_zero(counter_block.data(), counter_block.size());
#endif
}

}

#endif
