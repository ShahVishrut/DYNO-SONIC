#pragma once

#include "sonic/crypto/impl/openssl/prf_backend_config.hpp"

#if SONIC_OPENSSL_USE_LOWLEVEL

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/aes.h>

#include "sonic/util/span.hpp"

namespace sn::crypto::detail {

struct openssl_prf_lowlevel_state {
  AES_KEY key{};
#if SONIC_OPENSSL_HAS_AESNI
  alignas(16) std::array<std::uint8_t, 16 * (AES_MAXNR + 1)> round_keys{};
#endif
  int rounds = 0;
};

void openssl_prf_lowlevel_cleanup(openssl_prf_lowlevel_state& state);
void openssl_prf_lowlevel_set_key(openssl_prf_lowlevel_state& state, sn::util::span<const std::uint8_t> key_bytes);
void openssl_prf_lowlevel_stream(
    openssl_prf_lowlevel_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len
);

}

#endif
