#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/crypto/impl/bearssl/detail/backend.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto::detail {

void bearssl_prf_set_key(bearssl_prf_state& state, sn::util::span<const std::uint8_t> key);

void bearssl_prf_stream(bearssl_prf_state& state, const std::uint8_t* iv, std::uint8_t* out, std::size_t len);

void bearssl_prf_cleanup(bearssl_prf_state& state);

}
