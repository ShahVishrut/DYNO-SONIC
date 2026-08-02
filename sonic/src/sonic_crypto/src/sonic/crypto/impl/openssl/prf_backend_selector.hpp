#pragma once

#include "sonic/crypto/impl/openssl/prf_backend_config.hpp"

#if SONIC_OPENSSL_USE_LOWLEVEL
#include "sonic/crypto/impl/openssl/prf_backend_lowlevel.hpp"
#else
#include "sonic/crypto/impl/openssl/prf_backend_evp.hpp"
#endif

#include "sonic/util/span.hpp"

namespace sn::crypto::detail {

#if SONIC_OPENSSL_USE_LOWLEVEL

using openssl_prf_state = openssl_prf_lowlevel_state;

inline void openssl_prf_cleanup(openssl_prf_state& state) { openssl_prf_lowlevel_cleanup(state); }

inline void openssl_prf_set_key(openssl_prf_state& state, sn::util::span<const std::uint8_t> key_bytes) {
  openssl_prf_lowlevel_set_key(state, key_bytes);
}

inline void openssl_prf_stream(openssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  openssl_prf_lowlevel_stream(state, iv, out, len);
}

#else

using openssl_prf_state = openssl_prf_evp_state;

inline void openssl_prf_cleanup(openssl_prf_state& state) { openssl_prf_evp_cleanup(state); }

inline void openssl_prf_set_key(openssl_prf_state& state, sn::util::span<const std::uint8_t> key_bytes) {
  openssl_prf_evp_set_key(state, key_bytes);
}

inline void openssl_prf_stream(openssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len) {
  openssl_prf_evp_stream(state, iv, out, len);
}

#endif

}
