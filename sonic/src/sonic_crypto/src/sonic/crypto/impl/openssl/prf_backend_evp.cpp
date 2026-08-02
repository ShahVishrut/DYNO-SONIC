#include "sonic/crypto/impl/openssl/prf_backend_evp.hpp"

#if !SONIC_OPENSSL_USE_LOWLEVEL

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include <openssl/evp.h>

#include "sonic/crypto/error.hpp"

namespace sn::crypto::detail {

namespace {
constexpr std::size_t kEvpMaxRequest = static_cast<std::size_t>(std::numeric_limits<int>::max());
constexpr std::size_t kZeroBufferSize = detail::max_chunk_size;
alignas(16) const std::array<std::uint8_t, kZeroBufferSize> kZeroBuffer{};

inline std::size_t clamp_chunk(std::size_t remaining) {
  return std::min<std::size_t>({remaining, kZeroBufferSize, detail::max_chunk_size, kEvpMaxRequest});
}
}

void openssl_prf_evp_cleanup(openssl_prf_evp_state& state) {
  if (state.ctx.get() != nullptr) {
    EVP_CIPHER_CTX_reset(state.ctx.get());
  }
}

void openssl_prf_evp_set_key(openssl_prf_evp_state& state, sn::util::span<const std::uint8_t> key_bytes) {
  if (key_bytes.size() != 32) {
    throw error("openssl prf key must be 32 bytes");
  }

  detail::check_evp(EVP_CIPHER_CTX_reset(state.ctx.get()), "EVP_CIPHER_CTX_reset");
  detail::check_evp(
      EVP_EncryptInit_ex(state.ctx.get(), EVP_aes_256_ctr(), nullptr, key_bytes.data(), nullptr), "EVP_EncryptInit_ex"
  );
  detail::check_evp(EVP_CIPHER_CTX_set_padding(state.ctx.get(), 0), "EVP_CIPHER_CTX_set_padding");
}

void openssl_prf_evp_stream(openssl_prf_evp_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  if (len == 0) {
    return;
  }

  detail::check_evp(
      EVP_EncryptInit_ex(state.ctx.get(), nullptr, nullptr, nullptr, iv), "EVP_EncryptInit_ex reuse for prf"
  );

  std::size_t remaining = len;
  std::uint8_t* cursor = out;

  while (remaining > 0) {
    const std::size_t chunk = clamp_chunk(remaining);
    int produced = 0;
    detail::check_evp(
        EVP_EncryptUpdate(state.ctx.get(), cursor, &produced, kZeroBuffer.data(), static_cast<int>(chunk)),
        "EVP_EncryptUpdate prf"
    );
    if (produced != static_cast<int>(chunk)) {
      detail::throw_openssl_error("openssl prf short block");
    }
    cursor += chunk;
    remaining -= chunk;
  }

  int final_bytes = 0;
  std::array<std::uint8_t, 16> final_block{};
  detail::check_evp(EVP_EncryptFinal_ex(state.ctx.get(), final_block.data(), &final_bytes), "EVP_EncryptFinal_ex prf");
  if (final_bytes != 0) {
    detail::throw_openssl_error("openssl prf unexpected final bytes");
  }
  detail::secure_zero(final_block.data(), final_block.size());
}

}

#endif
