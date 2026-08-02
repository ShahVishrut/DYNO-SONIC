#pragma once

#include "sonic/crypto/impl/openssl/prf_backend_config.hpp"

#if !SONIC_OPENSSL_USE_LOWLEVEL

#include <cstddef>
#include <cstdint>

#include "sonic/crypto/impl/openssl/detail/utils.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto::detail {

struct openssl_prf_evp_state {
  evp_cipher_ctx ctx{};
};

void openssl_prf_evp_cleanup(openssl_prf_evp_state& state);
void openssl_prf_evp_set_key(openssl_prf_evp_state& state, sn::util::span<const std::uint8_t> key_bytes);
void openssl_prf_evp_stream(openssl_prf_evp_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len);

}

#endif
