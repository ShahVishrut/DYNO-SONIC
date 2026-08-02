#include "sonic/crypto/impl/bearssl/prf_backend.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(SN_BEARSSL_USE_X86NI)
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/impl/bearssl/detail/utils.hpp"
#include "sonic/util/memcpy.hpp"

namespace sn::crypto::detail {

namespace {
constexpr std::size_t kMaxChunkSize = detail::max_chunk_size;

#if defined(SN_BEARSSL_USE_X86NI)
inline std::uint32_t swap_be32(std::uint32_t value) {
#if defined(_MSC_VER)
  return _byteswap_ulong(value);
#else
  return __builtin_bswap32(value);
#endif
}

inline void aesni_stream(const bearssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  if (len == 0) {
    return;
  }

  const unsigned rounds = state.rounds;
  const auto* round_keys = reinterpret_cast<const __m128i*>(state.round_keys.data());
  const __m128i iv_base = _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv));
  std::uint32_t counter = 0;
  std::uint8_t* cursor = out;
  std::size_t remaining = len;

  while (remaining >= 64) {
    __m128i x0 = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter + 0u)), 3);
    __m128i x1 = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter + 1u)), 3);
    __m128i x2 = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter + 2u)), 3);
    __m128i x3 = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter + 3u)), 3);

    x0 = _mm_xor_si128(x0, round_keys[0]);
    x1 = _mm_xor_si128(x1, round_keys[0]);
    x2 = _mm_xor_si128(x2, round_keys[0]);
    x3 = _mm_xor_si128(x3, round_keys[0]);
    for (unsigned round = 1; round < rounds; ++round) {
      x0 = _mm_aesenc_si128(x0, round_keys[round]);
      x1 = _mm_aesenc_si128(x1, round_keys[round]);
      x2 = _mm_aesenc_si128(x2, round_keys[round]);
      x3 = _mm_aesenc_si128(x3, round_keys[round]);
    }
    x0 = _mm_aesenclast_si128(x0, round_keys[rounds]);
    x1 = _mm_aesenclast_si128(x1, round_keys[rounds]);
    x2 = _mm_aesenclast_si128(x2, round_keys[rounds]);
    x3 = _mm_aesenclast_si128(x3, round_keys[rounds]);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(cursor + 0), x0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(cursor + 16), x1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(cursor + 32), x2);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(cursor + 48), x3);
    cursor += 64;
    remaining -= 64;
    counter += 4;
  }

  while (remaining >= 16) {
    __m128i block = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter)), 3);
    block = _mm_xor_si128(block, round_keys[0]);
    for (unsigned round = 1; round < rounds; ++round) {
      block = _mm_aesenc_si128(block, round_keys[round]);
    }
    block = _mm_aesenclast_si128(block, round_keys[rounds]);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(cursor), block);
    cursor += 16;
    remaining -= 16;
    ++counter;
  }

  if (remaining > 0) {
    __m128i block = _mm_insert_epi32(iv_base, static_cast<int>(swap_be32(counter)), 3);
    block = _mm_xor_si128(block, round_keys[0]);
    for (unsigned round = 1; round < rounds; ++round) {
      block = _mm_aesenc_si128(block, round_keys[round]);
    }
    block = _mm_aesenclast_si128(block, round_keys[rounds]);
    alignas(16) std::array<std::uint8_t, 16> keystream{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(keystream.data()), block);
    sn::mem::copy_bytes(cursor, keystream.data(), remaining);
    detail::secure_zero(keystream.data(), keystream.size());
  }
}
#endif

inline void bearssl_ctr_stream(bearssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  std::size_t remaining = len;
  std::uint8_t* cursor = out;
  std::uint32_t counter = 0;

  while (remaining > 0) {
    const std::size_t chunk = std::min<std::size_t>(remaining, kMaxChunkSize);
    sn::mem::fill_bytes(cursor, 0, chunk);
    counter = detail::bearssl_ctr_run(&state.keys, iv, counter, cursor, chunk);
    cursor += chunk;
    remaining -= chunk;
  }
}

}

void bearssl_prf_set_key(bearssl_prf_state& state, sn::util::span<const std::uint8_t> key) {
  if (key.size() != 32) {
    throw error("bearssl prf key must be 32 bytes");
  }
  detail::bearssl_ctr_init(&state.keys, key.data(), key.size());
#if defined(SN_BEARSSL_USE_X86NI)
  state.rounds = state.keys.num_rounds;
  const std::size_t used_bytes = static_cast<std::size_t>(state.rounds + 1) * 16;
  sn::mem::copy_bytes(state.round_keys.data(), state.keys.skey.skni, used_bytes);
  if (used_bytes < state.round_keys.size()) {
    sn::mem::fill_bytes(state.round_keys.data() + used_bytes, 0, state.round_keys.size() - used_bytes);
  }
#endif
}

void bearssl_prf_stream(bearssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  if (len == 0) {
    return;
  }

#if defined(SN_BEARSSL_USE_X86NI)
  aesni_stream(state, iv, out, len);
#else
  bearssl_ctr_stream(state, iv, out, len);
#endif
}

void bearssl_prf_cleanup(bearssl_prf_state& state) {
  detail::secure_zero(&state.keys, sizeof(state.keys));
#if defined(SN_BEARSSL_USE_X86NI)
  detail::secure_zero(state.round_keys.data(), state.round_keys.size());
  state.rounds = 0;
#endif
}

}
